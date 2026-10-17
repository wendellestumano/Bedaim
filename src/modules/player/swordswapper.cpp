#include "swordswapper.hpp"
#include "swapperstate.hpp"

#include "aimassist.hpp"
#include "core/InventoryAccess.hpp"
#include "../ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <cmath>

namespace {
bool finiteVec(const bedrocktools::sdk::Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool validBounds(const bedrocktools::sdk::AABB& bounds) {
    if (!finiteVec(bounds.min) || !finiteVec(bounds.max)) return false;
    const float height = bounds.max.y - bounds.min.y;
    const float width = bounds.max.x - bounds.min.x;
    const float depth = bounds.max.z - bounds.min.z;
    return height >= 0.25f && height <= 8.0f && width > 0.0f && width <= 5.0f
        && depth > 0.0f && depth <= 5.0f;
}

bedrocktools::sdk::Vec3 eyePositionFor(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    if (!player) return {};
    // Never let ShiftBlade override the mace while it owns a confirmed downward slam.

    if (swapperstate::maceFallPriority.load(std::memory_order_acquire)) {
        restoreIfOwned();
        return;
    }

    if (m_ownsSwap && inventory.selectedHotbarSlot(player) != m_swordSlot) {
        // User changed the slot manually. Stop owning the slot and honor the override
        // until the current target leaves the configured range.
        m_ownsSwap = false;
        m_swordSlot = -1;
        m_previousSlot = -1;
        m_userOverrodeSwap = true;
    }

    const std::string heldIdentifier = inventory.selectedHotbarIdentifier(player);
    const bool holdingConsumable =
        heldIdentifier.find("apple") != std::string::npos ||
        heldIdentifier.find("golden_carrot") != std::string::npos ||
        heldIdentifier.find("bread") != std::string::npos ||
        heldIdentifier.find("cooked_") != std::string::npos ||
        heldIdentifier.find("raw_") != std::string::npos ||
        heldIdentifier.find("beef") != std::string::npos ||
        heldIdentifier.find("porkchop") != std::string::npos ||
        heldIdentifier.find("chicken") != std::string::npos ||
        heldIdentifier.find("mutton") != std::string::npos ||
        heldIdentifier.find("rabbit") != std::string::npos ||
        heldIdentifier.find("stew") != std::string::npos ||
        heldIdentifier.find("melon_slice") != std::string::npos ||
        heldIdentifier.find("chorus_fruit") != std::string::npos;
    if (holdingConsumable) {
        restoreIfOwned();
        m_userOverrodeSwap = true;
        m_foodUseOverride = true;
        return;
    }
    if (m_foodUseOverride) {
        // Clear only the override introduced by the food guard; unrelated
        // manual hotbar overrides retain the existing behavior.
        m_foodUseOverride = false;
        m_userOverrodeSwap = false;
    }
    if (heldIdentifier.find("wind_charge") != std::string::npos) {
        // Explicit user selection beats the automatic sword swap. Keep the
        // override until the target leaves the configured range.
        restoreIfOwned();
        m_userOverrodeSwap = true;
        return;
    }

    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    auto* aim = (aimBase && aimBase->enabled) ? static_cast<AimAssistModule*>(aimBase) : nullptr;
    bool shouldUseSword = false;

    if (aim) {
        void* target = nullptr;
        AABB bounds{};
        float aimRange = 0.0f;
        shouldUseSword =
            aim->currentTargetSnapshot(target, bounds, aimRange) &&
            targetWithinRange(player, bounds, std::min(m_attackRange, aimRange));
    }

    if (!shouldUseSword) {
        restoreIfOwned();
        m_userOverrodeSwap = false;
        return;
    }
    if (m_userOverrodeSwap) return;

    const int swordSlot = inventory.findHotbarSlot(player, "_sword");
    const int selected = inventory.selectedHotbarSlot(player);
    if (swordSlot < 0 || selected < 0) return;
    if (selected == swordSlot) return;

    if (!m_ownsSwap && inventory.selectHotbarSlot(player, swordSlot) &&
        inventory.selectedHotbarSlot(player) == swordSlot) {
        m_ownsSwap = true;
        m_swordSlot = swordSlot;
        m_previousSlot = selected;
    }
}

void SwordSwapperModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackRange = std::clamp(j.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
    m_restoreSlot = j.value("m_restoreSlot", m_restoreSlot);
}

void SwordSwapperModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_attackRange"] = m_attackRange;
    j["m_restoreSlot"] = m_restoreSlot;
}
