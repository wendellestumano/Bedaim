#include "maceswapper.hpp"
#include "swapperstate.hpp"

#include "aimassist.hpp"
#include "core/InventoryAccess.hpp"
#include "core/GameHooks.hpp"
#include "../ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>

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
                       const bedrocktools::sdk::AABB& targetBounds,
                       float range) {
    if (!player || !validBounds(targetBounds)) return false;
    const auto eye = eyePositionFor(player);
    if (!finiteVec(eye)) return false;
    const bedrocktools::sdk::Vec3 nearest{
        std::clamp(eye.x, targetBounds.min.x, targetBounds.max.x),
        std::clamp(eye.y, targetBounds.min.y, targetBounds.max.y),
        std::clamp(eye.z, targetBounds.min.z, targetBounds.max.z)
    };
    const float dx = eye.x - nearest.x;
    const float dy = eye.y - nearest.y;
    const float dz = eye.z - nearest.z;
    const float distanceSq = dx * dx + dy * dy + dz * dz;
    const float safeRange = std::clamp(range, 2.0f, 6.0f);
    return std::isfinite(distanceSq) && distanceSq <= safeRange * safeRange;
}
}

MaceSwapperModule::MaceSwapperModule()
    : Module("Mace Swapper", "Temporarily equips a hotbar mace while falling toward the selected Aim Assist target.") {
    keybind = 0;
}

void MaceSwapperModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { processPlayerTick(event.player); });
}

void MaceSwapperModule::onEnable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    // Keep an active fall and slot ownership if the user toggles the module
    // off and on before the next player tick can restore the inventory.
    if (!m_fallPriorityActive) {
        m_peakY = 0.0f;
        m_hasPeakY = false;
        m_descentTicks = 0;
        m_stableTicks = 0;
        m_userOverrodeSlot = false;
        if (!m_ownsMaceSlot) {
            m_fallMaceSlot = -1;
            m_fallPreviousSlot = -1;
            m_waitingAfterEquip = false;
        }
    }
    m_nextAttack = {};
}

void MaceSwapperModule::onDisable() {
    // The next player tick restores the previous slot on the game thread.
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    m_nextAttack = {};
}

void MaceSwapperModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    auto& inventory = bedrocktools::core::InventoryAccess::get();

    auto resetFall = [&]() {
        m_fallPriorityActive = false;
        m_ownsMaceSlot = false;
        m_userOverrodeSlot = false;
        m_waitingAfterEquip = false;
        m_fallMaceSlot = -1;
        m_fallPreviousSlot = -1;
        m_descentTicks = 0;
        m_stableTicks = 0;
        m_hasPeakY = false;
        m_nextAttack = {};
        swapperstate::maceFallPriority.store(false, std::memory_order_release);
    };

    auto restoreOwnedSlot = [&](Player* currentPlayer) {
        if (currentPlayer && m_ownsMaceSlot && m_restoreSlot &&
            m_fallMaceSlot >= 0 && m_fallPreviousSlot >= 0 &&
            inventory.selectedHotbarSlot(currentPlayer) == m_fallMaceSlot &&
            m_fallPreviousSlot != m_fallMaceSlot) {
            inventory.selectHotbarSlot(currentPlayer, m_fallPreviousSlot);
        }
        m_ownsMaceSlot = false;
        m_userOverrodeSlot = false;
        m_waitingAfterEquip = false;
        m_fallMaceSlot = -1;
        m_fallPreviousSlot = -1;
    };

    if (!player) {
        resetFall();
        m_hasLastPlayerY = false;
        return;
    }

    const Vec3 position = player->position();
    if (!finiteVec(position)) {
        m_hasLastPlayerY = false;
        return;
    }

    const float verticalDelta = m_hasLastPlayerY ? position.y - m_lastPlayerY : 0.0f;
    m_lastPlayerY = position.y;
    m_hasLastPlayerY = true;

    if (!m_hasPeakY) {
        m_peakY = position.y;
        m_hasPeakY = true;
    }
    if (position.y > m_peakY) m_peakY = position.y;

    const float fallDistance = std::max(0.0f, m_peakY - position.y);
    // A tiny hop is not a mace slam. Require the configured drop and a confirmed downward sample before acquiring the hotbar slot.
    if (verticalDelta < -0.04f && fallDistance >= std::clamp(m_minimumFallDistance, 0.30f, 10.0f)) {
        m_descentTicks = std::min(m_descentTicks + 1, 8);
    } else if (verticalDelta >= -0.01f || fallDistance < std::clamp(m_minimumFallDistance, 0.30f, 10.0f)) {
        m_descentTicks = 0;
    }

    if (std::fabs(verticalDelta) <= 0.0035f)
        m_stableTicks = std::min(m_stableTicks + 1, 8);
    else
        m_stableTicks = 0;

    if (!enabled || !m_enableMaceSwapper) {
        restoreOwnedSlot(player);
        resetFall();
        m_lastPlayerY = position.y;
        m_hasLastPlayerY = true;
        return;
    }

    const std::string heldIdentifier = inventory.selectedHotbarIdentifier(player);
    const bool holdingWindCharge = heldIdentifier.find("wind_charge") != std::string::npos;

    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    auto* aim = (aimBase && aimBase->enabled) ? static_cast<AimAssistModule*>(aimBase) : nullptr;

    // A normal jump should not trigger a mace swap before a real smash fall.
    // Do not acquire the main hand while a Wind Charge is selected: its use and
    // the swapper would otherwise compete for the same slot on mobile.
    if (holdingWindCharge && !m_fallPriorityActive) m_descentTicks = 0;
    if (!m_fallPriorityActive && !holdingWindCharge &&
        m_descentTicks >= 1 && fallDistance >= std::clamp(m_minimumFallDistance, 0.30f, 10.0f) && aim &&
        inventory.findHotbarSlot(player, "mace") >= 0) {
        void* target = nullptr;
        AABB targetBounds{};
        float aimRange = 0.0f;
        if (aim->currentTargetSnapshot(target, targetBounds, aimRange) &&
            targetWithinRange(player, targetBounds, std::min(m_attackRange, aimRange))) {
            m_fallPriorityActive = true;
            m_userOverrodeSlot = false;
            m_nextAttack = {};
        }
    }

    swapperstate::maceFallPriority.store(m_fallPriorityActive, std::memory_order_release);

    // Once the descent ends and vertical motion has settled for several ticks,
    // relinquish the slot exactly once rather than alternating items each hit.
    if (m_fallPriorityActive && m_stableTicks >= 4) {
        restoreOwnedSlot(player);
        resetFall();
        m_lastPlayerY = position.y;
        m_hasLastPlayerY = true;
        m_peakY = position.y;
        m_hasPeakY = true;
        return;
    }

    // Rebase the peak whenever vertical motion has been stable and no slam owns
    // priority, so an old elevation cannot make a later tiny hop look like a fall.
    if (!m_fallPriorityActive && m_stableTicks >= 4) {
        m_peakY = position.y;
        m_hasPeakY = true;
        m_descentTicks = 0;
    }

    if (!m_fallPriorityActive || !aim) return;

    const int selected = inventory.selectedHotbarSlot(player);
    if (m_fallMaceSlot >= 0 && selected >= 0 && selected != m_fallMaceSlot) {
        // A manual item change wins: do not force the mace back into the hand.
        if (m_ownsMaceSlot || m_fallMaceSlot >= 0) {
            m_userOverrodeSlot = true;
            m_ownsMaceSlot = false;
        }
    }

    if (m_fallMaceSlot < 0 && !m_userOverrodeSlot) {
        const int maceSlot = inventory.findHotbarSlot(player, "mace");
        const int previousSlot = inventory.selectedHotbarSlot(player);
        if (maceSlot < 0 || previousSlot < 0) return;
        if (previousSlot != maceSlot && !inventory.selectHotbarSlot(player, maceSlot)) return;
        if (inventory.selectedHotbarSlot(player) != maceSlot) return;

        m_fallMaceSlot = maceSlot;
        m_fallPreviousSlot = previousSlot;
        m_ownsMaceSlot = previousSlot != maceSlot;
        m_waitingAfterEquip = true;
        m_nextAttack = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
        return;
    }

    if (m_userOverrodeSlot || m_fallMaceSlot < 0 ||
        inventory.selectedHotbarSlot(player) != m_fallMaceSlot) return;

    // Give Bedrock one tick to apply the equipment change before attacking.
    if (m_waitingAfterEquip) {
        m_waitingAfterEquip = false;
        m_nextAttack = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_nextAttack.time_since_epoch().count() != 0 && now < m_nextAttack) return;

    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    if (!aim->currentTargetSnapshot(target, targetBounds, aimRange) ||
        !targetWithinRange(player, targetBounds, std::min(m_attackRange, aimRange))) {
        m_nextAttack = now + std::chrono::milliseconds(100);
        return;
    }

    void* gameMode = bedrocktools::core::gamehooks::gameModeForPlayer(player);
    if (!gameMode) {
        m_nextAttack = now + std::chrono::milliseconds(100);
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
        gameMode, target, false, &hitPoint);
    m_nextAttack = now + std::chrono::milliseconds(
        static_cast<int>(dispatched ? intervalMs : 120.0f));
}

void MaceSwapperModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_enableMaceSwapper = j.value("m_enableMaceSwapper", m_enableMaceSwapper);
    m_attackRange = std::clamp(j.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
    m_minimumFallDistance = std::clamp(j.value("m_minimumFallDistance", m_minimumFallDistance), 0.30f, 10.0f);
    m_attackIntervalMs = std::clamp(j.value("m_attackIntervalMs", m_attackIntervalMs), 80.0f, 1000.0f);
    m_restoreSlot = j.value("m_restoreSlot", m_restoreSlot);
}

void MaceSwapperModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_enableMaceSwapper"] = m_enableMaceSwapper;
    j["m_attackRange"] = m_attackRange;
    j["m_minimumFallDistance"] = m_minimumFallDistance;
    j["m_attackIntervalMs"] = m_attackIntervalMs;
    j["m_restoreSlot"] = m_restoreSlot;
}
