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
    const Vec3 position = player->position();
    const AABB bounds = player->bounds();
    Vec3 eye = position;
    if (validBounds(bounds)) {
        eye.x = (bounds.min.x + bounds.max.x) * 0.5f;
        eye.y = bounds.min.y + (bounds.max.y - bounds.min.y) * 0.85f;
        eye.z = (bounds.min.z + bounds.max.z) * 0.5f;
    } else {
        eye.y += 1.5f;
    }
    return eye;
}

bool targetWithinRange(bedrocktools::sdk::Player* player,
                       const bedrocktools::sdk::AABB& bounds, float range) {
    if (!player || !validBounds(bounds)) return false;
    const auto eye = eyePositionFor(player);
    if (!finiteVec(eye)) return false;
    const bedrocktools::sdk::Vec3 nearest{
        std::clamp(eye.x, bounds.min.x, bounds.max.x),
        std::clamp(eye.y, bounds.min.y, bounds.max.y),
        std::clamp(eye.z, bounds.min.z, bounds.max.z)
    };
    const float dx = eye.x - nearest.x;
    const float dy = eye.y - nearest.y;
    const float dz = eye.z - nearest.z;
    const float distanceSq = dx * dx + dy * dy + dz * dz;
    const float safeRange = std::clamp(range, 1.0f, 16.0f);
    return std::isfinite(distanceSq) && distanceSq <= safeRange * safeRange;
}
}

SwordSwapperModule::SwordSwapperModule()
    : Module("Sword Swapper", "Equips a hotbar sword when an Aim Assist target is in the crosshair and the player is grounded.") {
    keybind = 0;
}

void SwordSwapperModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { processPlayerTick(event.player); });
}

void SwordSwapperModule::onEnable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    m_groundedTicks = 0;
    m_userOverrodeSwap = false;
    // Preserve ownership if the user re-enables the module before the next game
    // tick has had a chance to restore the temporarily selected sword.
}

void SwordSwapperModule::onDisable() {
    // Do not touch inventory from the UI callback thread. The next player tick
    // restores the old slot if this module still owns the sword selection.
}

void SwordSwapperModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    auto& inventory = bedrocktools::core::InventoryAccess::get();

    if (!player) {
        m_lastPlayerY = 0.0f;
        m_hasLastPlayerY = false;
        m_groundedTicks = 0;
        m_ownsSwap = false;
        m_swordSlot = -1;
        m_previousSlot = -1;
        return;
    }

    auto restoreIfOwned = [&]() {
        // This lambda is reached on every tick while the module is disabled,
        // without a target, or while Mace Swapper has priority. Do not read the
        // native inventory at all unless Sword Swapper currently owns a swap.
        if (!m_ownsSwap) {
            m_swordSlot = -1;
            m_previousSlot = -1;
            return;
        }
        const int current = inventory.selectedHotbarSlot(player);
        if (m_restoreSlot && current == m_swordSlot && m_previousSlot >= 0)
            inventory.selectHotbarSlot(player, m_previousSlot);
        m_ownsSwap = false;
        m_swordSlot = -1;
        m_previousSlot = -1;
    };

    if (!enabled) {
        restoreIfOwned();
        m_userOverrodeSwap = false;
        return;
    }

    const Vec3 position = player->position();
    if (!finiteVec(position)) {
        m_hasLastPlayerY = false;
        m_groundedTicks = 0;
        restoreIfOwned();
        return;
    }
    const float dy = m_hasLastPlayerY ? position.y - m_lastPlayerY : 0.0f;
    m_lastPlayerY = position.y;
    m_hasLastPlayerY = true;

    // Don't wait 5 ticks to consider a stable landing. On mobile this delayed
    // the sword swap by roughly a quarter second and often missed short hit windows.
    if (std::fabs(dy) <= 0.01f)
        m_groundedTicks = std::min(m_groundedTicks + 1, 3);
    else
        m_groundedTicks = 0;
    const bool grounded = m_groundedTicks >= 1 && dy >= -0.01f;

    // Do not mistake the jump apex for ground, and never let Sword Swapper
    // override the mace while the latter owns a confirmed downward slam.
    if (swapperstate::maceFallPriority.load(std::memory_order_acquire)) {
        restoreIfOwned();
        return;
    }

    int selectedSlotSnapshot = -1;
    if (m_ownsSwap) {
        selectedSlotSnapshot = inventory.selectedHotbarSlot(player);
        if (selectedSlotSnapshot != m_swordSlot) {
            // User changed the slot manually. Stop owning the slot and honor the override
            // until this target leaves the crosshair or the player leaves the ground.
            m_ownsSwap = false;
            m_swordSlot = -1;
            m_previousSlot = -1;
            m_userOverrodeSwap = true;
        }
    }

    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    auto* aim = (aimBase && aimBase->enabled) ? static_cast<AimAssistModule*>(aimBase) : nullptr;
    bool shouldUseSword = false;

    if (grounded && aim) {
        void* target = nullptr;
        AABB bounds{};
        float aimRange = 0.0f;
        shouldUseSword =
            aim->currentTargetSnapshot(target, bounds, aimRange) &&
            targetWithinRange(player, bounds, std::min(m_attackRange, aimRange)) &&
            aim->currentTargetInCrosshair(player, target, 0.42f);
    }

    if (!shouldUseSword) {
        restoreIfOwned();
        m_userOverrodeSwap = false;
        return;
    }
    if (m_userOverrodeSwap) return;

    // Once we own the sword slot and it is still selected, there is no reason to
    // resolve any ItemStack identifier or rescan the hotbar. Those native string
    // lookups ran every game tick while the sword was equipped and were a hot path
    // for this module. Keep the ownership check above, then exit immediately.
    const int selected = selectedSlotSnapshot >= 0
        ? selectedSlotSnapshot : inventory.selectedHotbarSlot(player);
    if (selected < 0) return;
    if (m_ownsSwap && selected == m_swordSlot) return;

    const int swordSlot = inventory.findHotbarSlot(player, "_sword");
    if (swordSlot < 0 || selected == swordSlot) return;

    // ItemStack description lookup is only needed if a swap would really happen.
    const std::string heldIdentifier = inventory.selectedHotbarIdentifier(player);
    if (heldIdentifier.find("wind_charge") != std::string::npos) {
        restoreIfOwned();
        m_userOverrodeSwap = true;
        return;
    }

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
    m_attackRange = std::clamp(j.value("m_attackRange", m_attackRange), 1.0f, 16.0f);
    m_restoreSlot = j.value("m_restoreSlot", m_restoreSlot);
}

void SwordSwapperModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_attackRange"] = m_attackRange;
    j["m_restoreSlot"] = m_restoreSlot;
}
