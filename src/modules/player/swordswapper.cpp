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
    const float safeRange = std::clamp(range, 2.0f, 6.0f);
    return std::isfinite(distanceSq) && distanceSq <= safeRange * safeRange;
}
}

SwordSwapperModule::SwordSwapperModule()
    : Module("BladeShift", "Selects a hotbar sword when a BedrockAim target is in the crosshair and the player is grounded.",
             "Sword Swapper") {
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
        const int current = inventory.selectedHotbarSlot(player);
        if (m_ownsSwap && m_restoreSlot && current == m_swordSlot && m_previousSlot >= 0)
            inventory.selectHotbarSlot(player, m_previousSlot);
        m_ownsSwap = false;
        m_swordSlot = -1;
        m_previousSlot = -1;
    };

    if (!enabled) {
        restoreIfOwned();
        m_userOverrodeSwap = false;
        m_foodUseOverride = false;
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

    // Allow normal client/server position quantization while requiring several
    // samples so a jump apex is not mistaken for ground.
    if (std::fabs(dy) <= 0.02f)
        m_groundedTicks = std::min(m_groundedTicks + 1, 6);
    else
        m_groundedTicks = 0;
    const bool grounded = m_groundedTicks >= 3 && dy >= -0.02f;

    // Do not mistake the jump apex for ground, and never let Sword Swapper
    // override the mace while the latter owns a confirmed downward slam.
    if (swapperstate::maceFallPriority.load(std::memory_order_acquire)) {
        restoreIfOwned();
        return;
    }

    if (m_ownsSwap && inventory.selectedHotbarSlot(player) != m_swordSlot) {
        // User changed the slot manually. Stop owning the slot and honor the override
        // until this target leaves the crosshair or the player leaves the ground.
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
        // override until the target leaves the crosshair/grounded condition.
        restoreIfOwned();
        m_userOverrodeSwap = true;
        return;
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
            aim->currentTargetInCrosshair(player, target, 0.35f);
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
