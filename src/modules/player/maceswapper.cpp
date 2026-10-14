#include "maceswapper.hpp"

#include "aimassist.hpp"
#include "core/InventoryAccess.hpp"
#include "core/GameHooks.hpp"
#include "../ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

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

float distanceSquared(const bedrocktools::sdk::Vec3& a, const bedrocktools::sdk::Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bedrocktools::sdk::Vec3 eyePositionFor(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    const Vec3 position = player->position();
    const AABB bounds = player->bounds();
    Vec3 eyePosition = position;
    if (validBounds(bounds)) {
        eyePosition.x = (bounds.min.x + bounds.max.x) * 0.5f;
        eyePosition.y = bounds.min.y + (bounds.max.y - bounds.min.y) * 0.85f;
        eyePosition.z = (bounds.min.z + bounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }
    return eyePosition;
}

bool targetWithinRange(bedrocktools::sdk::Player* player,
                       const bedrocktools::sdk::AABB& targetBounds,
                       float range) {
    if (!player || !validBounds(targetBounds)) return false;
    const auto eye = eyePositionFor(player);
    if (!finiteVec(eye)) return false;
    const bedrocktools::sdk::Vec3 hitPoint{
        std::clamp(eye.x, targetBounds.min.x, targetBounds.max.x),
        std::clamp(eye.y, targetBounds.min.y, targetBounds.max.y),
        std::clamp(eye.z, targetBounds.min.z, targetBounds.max.z)
    };
    const float distanceSq = distanceSquared(eye, hitPoint);
    const float safeRange = std::clamp(range, 2.0f, 6.0f);
    return std::isfinite(distanceSq) && distanceSq <= safeRange * safeRange;
}
}

MaceSwapperModule::MaceSwapperModule()
    : Module("Mace Swapper", "Automatic mace-on-fall and optional sword-on-ground swapping for the current Aim Assist target.") {
    keybind = 0;
}

void MaceSwapperModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) {
            processPlayerTick(event.player);
        });
}

void MaceSwapperModule::onEnable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    m_groundedTicks = 0;
    m_nextAttack = {};
    m_pendingAttack = false;
    m_pendingMaceSlot = -1;
    m_pendingPreviousSlot = -1;
    m_pendingTicks = 0;
    m_pendingTarget = nullptr;
    m_pendingPlayer = nullptr;
    m_swordSwapOwned = false;
    m_swordSlot = -1;
    m_swordPreviousSlot = -1;
}

void MaceSwapperModule::onDisable() {
    // Keep pending slot ownership until the next LocalPlayerTickEvent so a still-
    // selected temporary weapon can be restored safely from the game thread.
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    m_groundedTicks = 0;
    m_nextAttack = {};
}

void MaceSwapperModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    auto& inventory = bedrocktools::core::InventoryAccess::get();

    if (!player) {
        m_hasLastPlayerY = false;
        m_groundedTicks = 0;
        m_pendingAttack = false;
        m_pendingTarget = nullptr;
        m_pendingPlayer = nullptr;
        m_swordSwapOwned = false;
        m_swordSlot = -1;
        m_swordPreviousSlot = -1;
        return;
    }

    const Vec3 playerPosition = player->position();
    if (!finiteVec(playerPosition)) {
        m_hasLastPlayerY = false;
        m_groundedTicks = 0;
        return;
    }

    const float verticalDelta = m_hasLastPlayerY ? playerPosition.y - m_lastPlayerY : 0.0f;
    m_lastPlayerY = playerPosition.y;
    m_hasLastPlayerY = true;

    // BedrockTools' public wrapper does not expose a stable grounded flag for all
    // supported builds. Require two stationary player ticks rather than treating the
    // single zero-velocity tick at a jump apex as grounded.
    if (std::fabs(verticalDelta) <= 0.0025f) {
        m_groundedTicks = std::min(m_groundedTicks + 1, 4);
    } else {
        m_groundedTicks = 0;
    }
    const bool grounded = m_groundedTicks >= 2;
    const bool falling = verticalDelta < -0.025f;

    auto restoreSwordIfOwned = [&]() {
        if (m_swordSwapOwned) {
            const int selectedSlot = inventory.selectedHotbarSlot(player);
            if (m_restoreSlot && selectedSlot == m_swordSlot && m_swordPreviousSlot >= 0) {
                inventory.selectHotbarSlot(player, m_swordPreviousSlot);
            }
        }
        m_swordSwapOwned = false;
        m_swordSlot = -1;
        m_swordPreviousSlot = -1;
    };

    auto clearPendingMace = [&]() {
        m_pendingAttack = false;
        m_pendingMaceSlot = -1;
        m_pendingPreviousSlot = -1;
        m_pendingTicks = 0;
        m_pendingTarget = nullptr;
        m_pendingPlayer = nullptr;
    };

    auto restoreMaceIfOwned = [&]() {
        const int selectedSlot = inventory.selectedHotbarSlot(player);
        if (m_restoreSlot && selectedSlot == m_pendingMaceSlot && m_pendingPreviousSlot >= 0) {
            inventory.selectHotbarSlot(player, m_pendingPreviousSlot);
        }
        clearPendingMace();
    };

    if (!enabled) {
        if (m_pendingAttack && player == m_pendingPlayer) restoreMaceIfOwned();
        restoreSwordIfOwned();
        return;
    }

    // Resolve a queued hit first. The selected target is matched against a fresh
    // engine actor snapshot before any bounds are read, protecting against despawns.
    if (m_pendingAttack) {
        const int selectedSlot = inventory.selectedHotbarSlot(player);
        const bool stillHoldingMace = selectedSlot == m_pendingMaceSlot;
        if (!m_enableMaceSwapper || player != m_pendingPlayer || !stillHoldingMace) {
            restoreMaceIfOwned();
            return;
        }

        auto* aimModuleBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
        if (!aimModuleBase || !aimModuleBase->enabled) {
            restoreMaceIfOwned();
            return;
        }
        auto* aim = static_cast<AimAssistModule*>(aimModuleBase);
        AABB targetBounds{};
        float aimRange = 0.0f;
        if (!aim->targetBoundsSnapshot(m_pendingTarget, targetBounds, aimRange) ||
            !targetWithinRange(player, targetBounds, std::min(m_attackRange, aimRange))) {
            restoreMaceIfOwned();
            return;
        }

        void* gameMode = bedrocktools::core::gamehooks::gameMode();
        if (!gameMode) {
            // The engine hook learns the valid GameMode instance through real engine
            // callbacks. Give it a couple of ticks to become available instead of
            // cancelling the swap immediately on a transient null.
            if (++m_pendingTicks <= 3) return;
            restoreMaceIfOwned();
            return;
        }

        const Vec3 eye = eyePositionFor(player);
        const Vec3 hitPoint{
            std::clamp(eye.x, targetBounds.min.x, targetBounds.max.x),
            std::clamp(eye.y, targetBounds.min.y, targetBounds.max.y),
            std::clamp(eye.z, targetBounds.min.z, targetBounds.max.z)
        };
        const float intervalMs = std::clamp(m_attackIntervalMs, 80.0f, 1000.0f);
        const bool dispatched = bedrocktools::core::gamehooks::attack(
            gameMode, m_pendingTarget, true, &hitPoint);
        restoreMaceIfOwned();
        if (dispatched) {
            m_nextAttack = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(static_cast<int>(intervalMs));
            aim->deferAutoHitFor(intervalMs);
        }
        return;
    }

    // Sword swapper is optional and may operate while Mace Swapper is disabled.
    // It requires a stable grounded state and the Aim Assist's selected target to
    // intersect the current view ray. It won't overwrite a manual hotbar change.
    bool swordTargetInCrosshair = false;
    int swordSlot = -1;
    auto* aimModuleBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    auto* aim = (aimModuleBase && aimModuleBase->enabled)
        ? static_cast<AimAssistModule*>(aimModuleBase) : nullptr;

    if (m_enableSwordSwapper && grounded && !falling && aim) {
        void* target = nullptr;
        AABB targetBounds{};
        float aimRange = 0.0f;
        if (aim->currentTargetSnapshot(target, targetBounds, aimRange) &&
            targetWithinRange(player, targetBounds, std::min(m_attackRange, aimRange)) &&
            aim->currentTargetInCrosshair(player, target)) {
            swordTargetInCrosshair = true;
            swordSlot = inventory.findHotbarSlot(player, "sword");
        }
    }

    if (m_swordSwapOwned && inventory.selectedHotbarSlot(player) != m_swordSlot) {
        // The user changed slots manually; relinquish ownership without restoring.
        m_swordSwapOwned = false;
        m_swordSlot = -1;
        m_swordPreviousSlot = -1;
    }

    if (swordTargetInCrosshair && swordSlot >= 0) {
        const int currentSlot = inventory.selectedHotbarSlot(player);
        if (currentSlot >= 0 && currentSlot != swordSlot && !m_swordSwapOwned) {
            if (inventory.selectHotbarSlot(player, swordSlot) &&
                inventory.selectedHotbarSlot(player) == swordSlot) {
                m_swordSwapOwned = true;
                m_swordSlot = swordSlot;
                m_swordPreviousSlot = currentSlot;
            }
        }
    } else {
        restoreSwordIfOwned();
    }

    if (!m_enableMaceSwapper || !falling) return;

    const auto now = std::chrono::steady_clock::now();
    if (m_nextAttack.time_since_epoch().count() != 0 && now < m_nextAttack) return;
    if (!aim) return;

    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    if (!aim->currentTargetSnapshot(target, targetBounds, aimRange)) return;
    // Deliberately do not require a perfect crosshair ray on the falling tick. The
    // Aim Assist has already selected this target; its angle can still be converging
    // during a Wind Burst drop. Range and a fresh snapshot remain mandatory.
    if (!targetWithinRange(player, targetBounds, std::min(m_attackRange, aimRange))) return;

    const int maceSlot = inventory.findHotbarSlot(player, "mace");
    const int previousSlot = inventory.selectedHotbarSlot(player);
    if (maceSlot < 0 || previousSlot < 0) return;
    if (previousSlot != maceSlot && !inventory.selectHotbarSlot(player, maceSlot)) return;
    if (inventory.selectedHotbarSlot(player) != maceSlot) return;

    m_pendingMaceSlot = maceSlot;
    m_pendingPreviousSlot = previousSlot;
    m_pendingTicks = 0;
    m_pendingTarget = target;
    m_pendingPlayer = player;
    m_pendingAttack = true;

    // Throttle the Aim Assist attack on the following tick so it doesn't steal the
    // mace swing before this queued weapon swap has had one tick to settle.
    aim->deferAutoHitFor(m_attackIntervalMs);
}

void MaceSwapperModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_enableMaceSwapper = j.value("m_enableMaceSwapper", m_enableMaceSwapper);
    m_enableSwordSwapper = j.value("m_enableSwordSwapper", m_enableSwordSwapper);
    m_attackRange = std::clamp(j.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
    m_attackIntervalMs = std::clamp(j.value("m_attackIntervalMs", m_attackIntervalMs), 80.0f, 1000.0f);
    m_restoreSlot = j.value("m_restoreSlot", m_restoreSlot);
}

void MaceSwapperModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_enableMaceSwapper"] = m_enableMaceSwapper;
    j["m_enableSwordSwapper"] = m_enableSwordSwapper;
    j["m_attackRange"] = m_attackRange;
    j["m_attackIntervalMs"] = m_attackIntervalMs;
    j["m_restoreSlot"] = m_restoreSlot;
}
