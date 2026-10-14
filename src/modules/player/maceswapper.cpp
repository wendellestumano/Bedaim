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

float distanceSquared(const bedrocktools::sdk::Vec3& a, const bedrocktools::sdk::Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}
}

MaceSwapperModule::MaceSwapperModule()
    : Module("Mace Swapper", "Swaps to a hotbar mace for a falling hit when the AimAssist target is in the crosshair.") {
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
    m_nextAttack = {};
}

void MaceSwapperModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    m_nextAttack = {};
}

void MaceSwapperModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> lock(m_stateMutex);

    if (!enabled || !player) {
        m_hasLastPlayerY = false;
        return;
    }

    const Vec3 playerPosition = player->position();
    if (!finiteVec(playerPosition)) {
        m_hasLastPlayerY = false;
        return;
    }

    const float verticalDelta = m_hasLastPlayerY ? playerPosition.y - m_lastPlayerY : 0.0f;
    m_lastPlayerY = playerPosition.y;
    m_hasLastPlayerY = true;

    // Require actual downward movement; rising jumps and standing still do not trigger.
    if (verticalDelta >= -0.025f) return;

    const auto now = std::chrono::steady_clock::now();
    if (m_nextAttack.time_since_epoch().count() != 0 && now < m_nextAttack) return;

    auto* aimModuleBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (!aimModuleBase || !aimModuleBase->enabled) return;
    auto* aim = static_cast<AimAssistModule*>(aimModuleBase);

    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    if (!aim->currentTargetSnapshot(target, targetBounds, aimRange)) return;
    if (!aim->currentTargetInCrosshair(player, target)) return;

    void* gameMode = field<void*>(player, offsets::Player::mGameMode);
    if (!gameMode) gameMode = bedrocktools::core::gamehooks::gameMode();
    if (!gameMode || !validBounds(targetBounds)) return;

    const AABB playerBounds = player->bounds();
    Vec3 eyePosition = playerPosition;
    if (validBounds(playerBounds)) {
        eyePosition.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        eyePosition.y = playerBounds.min.y + (playerBounds.max.y - playerBounds.min.y) * 0.85f;
        eyePosition.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }

    const Vec3 hitPoint{
        std::clamp(eyePosition.x, targetBounds.min.x, targetBounds.max.x),
        std::clamp(eyePosition.y, targetBounds.min.y, targetBounds.max.y),
        std::clamp(eyePosition.z, targetBounds.min.z, targetBounds.max.z)
    };
    const float effectiveRange = std::min(
        std::clamp(m_attackRange, 2.0f, 6.0f),
        std::clamp(aimRange, 2.0f, 80.0f));
    const float hitDistanceSq = distanceSquared(eyePosition, hitPoint);
    if (!std::isfinite(hitDistanceSq) || hitDistanceSq < 0.01f ||
        hitDistanceSq > effectiveRange * effectiveRange) return;

    auto& inventory = bedrocktools::core::InventoryAccess::get();
    const int maceSlot = inventory.findHotbarSlot(player, "mace");
    const int previousSlot = inventory.selectedHotbarSlot(player);
    if (maceSlot < 0 || previousSlot < 0) return;

    const float intervalMs = std::clamp(m_attackIntervalMs, 80.0f, 1000.0f);
    bool switched = false;
    if (previousSlot != maceSlot) {
        if (!inventory.selectHotbarSlot(player, maceSlot)) return;
        switched = true;
    }

    // The GameMode call consumes the selected hotbar item synchronously. Restore
    // the previous selection immediately after the native attack path returns.
    bedrocktools::core::gamehooks::attack(gameMode, target, true, &hitPoint);
    if (switched && m_restoreSlot) inventory.selectHotbarSlot(player, previousSlot);

    m_nextAttack = now + std::chrono::milliseconds(static_cast<int>(intervalMs));
    aim->deferAutoHitFor(intervalMs);
}

void MaceSwapperModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackRange = std::clamp(j.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
    m_attackIntervalMs = std::clamp(j.value("m_attackIntervalMs", m_attackIntervalMs), 80.0f, 1000.0f);
    m_restoreSlot = j.value("m_restoreSlot", m_restoreSlot);
}

void MaceSwapperModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_attackRange"] = m_attackRange;
    j["m_attackIntervalMs"] = m_attackIntervalMs;
    j["m_restoreSlot"] = m_restoreSlot;
}
