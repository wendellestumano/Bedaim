#include "maceswapper.hpp"

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
    m_nextAttack = {};
    m_pendingAttack = false;
    m_pendingMaceSlot = -1;
    m_pendingPreviousSlot = -1;
    m_pendingTicks = 0;
    m_pendingTarget = nullptr;
    m_pendingPlayer = nullptr;
}

void MaceSwapperModule::onDisable() {
    // Restoration is completed on the next game-thread tick, not from the UI thread.
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    m_nextAttack = {};
}

void MaceSwapperModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    auto& inventory = bedrocktools::core::InventoryAccess::get();

    if (!player) {
        m_hasLastPlayerY = false;
        m_pendingAttack = false;
        m_pendingTarget = nullptr;
        m_pendingPlayer = nullptr;
        m_pendingMaceSlot = -1;
        m_pendingPreviousSlot = -1;
        m_pendingTicks = 0;
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

    auto clearPending = [&]() {
        m_pendingAttack = false;
        m_pendingMaceSlot = -1;
        m_pendingPreviousSlot = -1;
        m_pendingTicks = 0;
        m_pendingTarget = nullptr;
        m_pendingPlayer = nullptr;
    };
    auto restoreMaceIfOwned = [&]() {
        const int selected = inventory.selectedHotbarSlot(player);
        if (m_restoreSlot && selected == m_pendingMaceSlot && m_pendingPreviousSlot >= 0) {
            inventory.selectHotbarSlot(player, m_pendingPreviousSlot);
        }
        clearPending();
    };

    if (m_pendingAttack) {
        const int selected = inventory.selectedHotbarSlot(player);
        if (!enabled || !m_enableMaceSwapper || player != m_pendingPlayer ||
            selected != m_pendingMaceSlot) {
            restoreMaceIfOwned();
            return;
        }

        auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
        if (!aimBase || !aimBase->enabled) {
            restoreMaceIfOwned();
            return;
        }
        auto* aim = static_cast<AimAssistModule*>(aimBase);
        AABB targetBounds{};
        float aimRange = 0.0f;
        if (!aim->targetBoundsSnapshot(m_pendingTarget, targetBounds, aimRange) ||
            !targetWithinRange(player, targetBounds, std::min(m_attackRange, aimRange))) {
            restoreMaceIfOwned();
            return;
        }

        void* gameMode = bedrocktools::core::gamehooks::gameMode();
        if (!gameMode) {
            if (++m_pendingTicks <= 2) return;
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

    if (!enabled || !m_enableMaceSwapper || verticalDelta >= -0.025f) return;
    const auto now = std::chrono::steady_clock::now();
    if (m_nextAttack.time_since_epoch().count() != 0 && now < m_nextAttack) return;

    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (!aimBase || !aimBase->enabled) return;
    auto* aim = static_cast<AimAssistModule*>(aimBase);

    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    if (!aim->currentTargetSnapshot(target, targetBounds, aimRange)) return;
    // Aim Assist already selected the target; do not add a second strict ray test
    // at the start of a fast fall, when Wind Burst may still be moving the camera.
    if (!targetWithinRange(player, targetBounds, std::min(m_attackRange, aimRange))) return;

    const int maceSlot = inventory.findHotbarSlot(player, "mace");
    const int previousSlot = inventory.selectedHotbarSlot(player);
    if (maceSlot < 0 || previousSlot < 0) return;
    if (previousSlot != maceSlot && !inventory.selectHotbarSlot(player, maceSlot)) return;
    if (inventory.selectedHotbarSlot(player) != maceSlot) {
        if (m_restoreSlot && previousSlot >= 0) inventory.selectHotbarSlot(player, previousSlot);
        return;
    }

    m_pendingMaceSlot = maceSlot;
    m_pendingPreviousSlot = previousSlot;
    m_pendingTicks = 0;
    m_pendingTarget = target;
    m_pendingPlayer = player;
    m_pendingAttack = true;
    aim->deferAutoHitFor(m_attackIntervalMs);
}

void MaceSwapperModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_enableMaceSwapper = j.value("m_enableMaceSwapper", m_enableMaceSwapper);
    m_attackRange = std::clamp(j.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
    m_attackIntervalMs = std::clamp(j.value("m_attackIntervalMs", m_attackIntervalMs), 80.0f, 1000.0f);
    m_restoreSlot = j.value("m_restoreSlot", m_restoreSlot);
}

void MaceSwapperModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_enableMaceSwapper"] = m_enableMaceSwapper;
    j["m_attackRange"] = m_attackRange;
    j["m_attackIntervalMs"] = m_attackIntervalMs;
    j["m_restoreSlot"] = m_restoreSlot;
}
