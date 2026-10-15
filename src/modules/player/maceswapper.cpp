#include "maceswapper.hpp"
#include "swapperstate.hpp"

#include "aimassist.hpp"
#include "core/InventoryAccess.hpp"
#include "core/GameHooks.hpp"
#include "../ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/world/Dimension.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

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

using BlockSourceIsSolidBlockingBlockFn = bool (*)(void*, const bedrocktools::sdk::BlockPos*);

BlockSourceIsSolidBlockingBlockFn solidBlockingBlockQuery() {
    static const auto query = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock));
    return query;
}

// Measure vertical clearance to the first solid block under the player's own
// X/Z column. The query range is deliberately bounded and fails closed when
// world/block data is unavailable instead of substituting target height.
float heightAboveGround(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    if (!player) return std::numeric_limits<float>::quiet_NaN();
    auto* dimension = player->dimension();
    auto* source = dimension ? dimension->blockSource() : nullptr;
    const auto isSolid = solidBlockingBlockQuery();
    if (!source || !isSolid) return std::numeric_limits<float>::quiet_NaN();

    const Vec3 position = player->position();
    const AABB bounds = player->bounds();
    if (!finiteVec(position)) return std::numeric_limits<float>::quiet_NaN();
    const float feetY = validBounds(bounds) ? bounds.min.y : position.y;
    const float centerX = validBounds(bounds) ? (bounds.min.x + bounds.max.x) * 0.5f : position.x;
    const float centerZ = validBounds(bounds) ? (bounds.min.z + bounds.max.z) * 0.5f : position.z;
    if (!std::isfinite(feetY) || !std::isfinite(centerX) || !std::isfinite(centerZ) ||
        feetY < -30000000.0f || feetY > 30000000.0f ||
        std::abs(centerX) > 30000000.0f || std::abs(centerZ) > 30000000.0f) {
        return std::numeric_limits<float>::quiet_NaN();
    }

    const int blockX = static_cast<int>(std::floor(centerX));
    const int blockZ = static_cast<int>(std::floor(centerZ));
    int blockY = static_cast<int>(std::floor(feetY)) - 1;
    constexpr int kGroundSearchDepth = 12;
    for (int step = 0; step < kGroundSearchDepth; ++step, --blockY) {
        const BlockPos pos{blockX, blockY, blockZ};
        if (!isSolid(source, &pos)) continue;
        const float height = feetY - static_cast<float>(blockY + 1);
        return std::isfinite(height) ? std::max(0.0f, height)
                                     : std::numeric_limits<float>::quiet_NaN();
    }
    return std::numeric_limits<float>::quiet_NaN();
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
    // Overlap with the player's eye still counts as in range.
    return std::isfinite(distanceSq) && distanceSq <= safeRange * safeRange;
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
    m_pendingAttack = false;
            swapperstate::maceFallPriority.store(false, std::memory_order_release);
    m_pendingMaceSlot = -1;
    m_pendingPreviousSlot = -1;
    m_pendingTarget = nullptr;
    m_pendingPlayer = nullptr;
}

void MaceSwapperModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    // If we were between the hotbar change and the hit, leave restoration pending
    // for the next player tick; it can safely restore only if the mace is still selected.
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    m_nextAttack = {};
}

void MaceSwapperModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> lock(m_stateMutex);

    if (!player) {
        m_hasLastPlayerY = false;
        m_pendingAttack = false;
            swapperstate::maceFallPriority.store(false, std::memory_order_release);
        m_pendingTarget = nullptr;
        m_pendingPlayer = nullptr;
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

    auto& inventory = bedrocktools::core::InventoryAccess::get();

    // Finish a swap on the following tick. This gives vanilla a tick to observe
    // the selected mace before attack and slot restoration, rather than changing
    // the selected slot twice in the same callback where no equip update can occur.
    if (m_pendingAttack) {
        const int selectedSlot = inventory.selectedHotbarSlot(player);
        const bool stillHoldingMace = selectedSlot == m_pendingMaceSlot;
        auto restoreIfStillSwapped = [&]() {
            if (m_restoreSlot && stillHoldingMace && m_pendingPreviousSlot >= 0) {
                inventory.selectHotbarSlot(player, m_pendingPreviousSlot);
            }
            m_pendingAttack = false;
            swapperstate::maceFallPriority.store(false, std::memory_order_release);
            m_pendingMaceSlot = -1;
            m_pendingPreviousSlot = -1;
            m_pendingTarget = nullptr;
            m_pendingPlayer = nullptr;
        };

        if (!enabled) {
            restoreIfStillSwapped();
            return;
        }
        if (player != m_pendingPlayer || !stillHoldingMace) {
            restoreIfStillSwapped();
            return;
        }

        auto* aimModuleBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
        if (!aimModuleBase || !aimModuleBase->enabled) {
            restoreIfStillSwapped();
            return;
        }
        auto* aim = static_cast<AimAssistModule*>(aimModuleBase);
        void* currentTarget = nullptr;
        AABB targetBounds{};
        float aimRange = 0.0f;
        if (!aim->currentTargetSnapshot(currentTarget, targetBounds, aimRange) ||
            currentTarget != m_pendingTarget ||
            !targetWithinRange(player, targetBounds, std::min(m_attackRange, aimRange))) {
            restoreIfStillSwapped();
            return;
        }

        void* gameMode = bedrocktools::core::gamehooks::gameModeForPlayer(player);
        if (!gameMode || !bedrocktools::core::gamehooks::attackAvailable(gameMode)) {
            restoreIfStillSwapped();
            return;
        }

        const Vec3 eye = eyePositionFor(player);
        const Vec3 hitPoint{
            std::clamp(eye.x, targetBounds.min.x, targetBounds.max.x),
            std::clamp(eye.y, targetBounds.min.y, targetBounds.max.y),
            std::clamp(eye.z, targetBounds.min.z, targetBounds.max.z)
        };
        const float intervalMs = std::clamp(m_attackIntervalMs, 80.0f, 1000.0f);
        const bool attacked = bedrocktools::core::gamehooks::attack(gameMode, currentTarget, true, &hitPoint);
        restoreIfStillSwapped();
        if (attacked) {
            m_nextAttack = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(static_cast<int>(intervalMs));
            aim->deferAutoHitFor(intervalMs);
        }
        return;
    }

    if (!enabled || verticalDelta >= -0.025f) return;

    const auto now = std::chrono::steady_clock::now();
    if (m_nextAttack.time_since_epoch().count() != 0 && now < m_nextAttack) return;

    auto* aimModuleBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (!aimModuleBase || !aimModuleBase->enabled) return;
    auto* aim = static_cast<AimAssistModule*>(aimModuleBase);

    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    if (!aim->currentTargetSnapshot(target, targetBounds, aimRange)) return;

    // Use the configured world-space height above solid ground, rather than
    // distance to the target AABB; uneven terrain and floating targets must not
    // change the user's height threshold.
    const float height = heightAboveGround(player);
    const float activationHeight = std::clamp(m_activationHeight, 1.7f, 6.0f);
    if (!std::isfinite(height) || height < activationHeight) return;

    if (!aim->currentTargetInCrosshair(player, target)) return;
    if (!targetWithinRange(player, targetBounds, std::min(m_attackRange, aimRange))) return;

    void* gameMode = bedrocktools::core::gamehooks::gameModeForPlayer(player);
    if (!gameMode || !bedrocktools::core::gamehooks::attackAvailable(gameMode)) return;

    const int maceSlot = inventory.findHotbarSlot(player, "mace");
    const int previousSlot = inventory.selectedHotbarSlot(player);
    if (maceSlot < 0 || previousSlot < 0) return;

    if (previousSlot != maceSlot && !inventory.selectHotbarSlot(player, maceSlot)) return;
    m_pendingMaceSlot = maceSlot;
    m_pendingPreviousSlot = previousSlot;
    m_pendingTarget = target;
    m_pendingPlayer = player;
    m_pendingAttack = true;
    swapperstate::maceFallPriority.store(true, std::memory_order_release);

    const float intervalMs = std::clamp(m_attackIntervalMs, 80.0f, 1000.0f);
    aim->deferAutoHitFor(intervalMs);
}

void MaceSwapperModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackRange = std::clamp(j.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
    m_activationHeight = std::clamp(j.value("m_activationHeight", m_activationHeight), 1.7f, 6.0f);
    m_attackIntervalMs = std::clamp(j.value("m_attackIntervalMs", m_attackIntervalMs), 80.0f, 1000.0f);
    m_restoreSlot = j.value("m_restoreSlot", m_restoreSlot);
}

void MaceSwapperModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_attackRange"] = m_attackRange;
    j["m_activationHeight"] = m_activationHeight;
    j["m_attackIntervalMs"] = m_attackIntervalMs;
    j["m_restoreSlot"] = m_restoreSlot;
}
