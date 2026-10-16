#include "maceswapper.hpp"
#include "MaceFallPolicy.hpp"
#include "swapperstate.hpp"

#include <pl/ModMenu.hpp>
#include <cctype>
#include <string>
#include <utility>

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
    // Overlap with the player's eye still counts as in range.
    return std::isfinite(distanceSq) && distanceSq <= safeRange * safeRange;
}
}

MaceSwapperModule::MaceSwapperModule()
    : Module("BedrockImpact", "BedrockImpact: context-aware hotbar switching for falling attacks.", "Mace Swapper") {
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
    m_peakY = 0.0f;
    m_hasPeakY = false;
    m_nextAttack = {};
    swapperstate::maceFallPriority.store(false, std::memory_order_release);
    m_pendingAttack = false;
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
    m_peakY = 0.0f;
    m_hasPeakY = false;
    m_nextAttack = {};
}

void MaceSwapperModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> lock(m_stateMutex);

    if (!player) {
        m_hasLastPlayerY = false;
        m_hasPeakY = false;
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

    // Track the peak of the current airborne segment. Rebase on level/rising
    // motion so an earlier jump cannot arm a later fall.
    if (!m_hasPeakY || verticalDelta >= -0.025f) {
        m_peakY = playerPosition.y;
        m_hasPeakY = true;
    }
    if (playerPosition.y > m_peakY) m_peakY = playerPosition.y;
    const float fallDistance = std::max(0.0f, m_peakY - playerPosition.y);

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

        if (!enabled || !m_enableMaceSwapper) {
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

        void* gameMode = bedrocktools::core::gamehooks::gameMode();
        if (!gameMode) {
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

    if (!enabled || !m_enableMaceSwapper ||
        !bedaim::macefall::shouldBeginMaceAction(verticalDelta, fallDistance, m_minimumFallDistance)) return;

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
    if (!targetWithinRange(player, targetBounds, std::min(m_attackRange, aimRange))) return;

    void* gameMode = bedrocktools::core::gamehooks::gameMode();
    if (!gameMode) return;

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
    m_enableMaceSwapper = j.value("m_enableMaceSwapper", m_enableMaceSwapper);
    m_attackRange = std::clamp(j.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
    m_minimumFallDistance = bedaim::macefall::normalizeMinimumFallDistance(j.value("m_minimumFallDistance", m_minimumFallDistance));
    m_attackIntervalMs = std::clamp(j.value("m_attackIntervalMs", m_attackIntervalMs), 80.0f, 1000.0f);
    m_restoreSlot = j.value("m_restoreSlot", m_restoreSlot);
}

void MaceSwapperModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_enableMaceSwapper"] = m_enableMaceSwapper;
    j["m_attackRange"] = m_attackRange;
    j["m_minimumFallDistance"] = bedaim::macefall::normalizeMinimumFallDistance(m_minimumFallDistance);
    j["m_attackIntervalMs"] = m_attackIntervalMs;
    j["m_restoreSlot"] = m_restoreSlot;
}

void MaceSwapperModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("combat")
        .category("combat", "Combat", "Mace swap and fall requirements");

    auto slider = [&](const char* key, const char* title,
                      const char* minimum, const char* maximum,
                      const char* step, const char* unit) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.title = title;
        node.category = "combat";
        node.type = ConfigControlTypeV2::SliderFloat;
        node.minValue = minimum;
        node.maxValue = maximum;
        node.step = step;
        node.unit = unit;
        schema.node(std::move(node));
    };

    ConfigNodeV2 enabledNode;
    enabledNode.id = "m_enableMaceSwapper";
    enabledNode.key = "m_enableMaceSwapper";
    enabledNode.title = "Enable Mace Swapper";
    enabledNode.category = "combat";
    enabledNode.type = ConfigControlTypeV2::Toggle;
    schema.node(std::move(enabledNode));

    slider("m_attackRange", "Target range", "2", "6", "0.1", " blocks");
    slider("m_minimumFallDistance", "Minimum fall distance", "0.30", "10.0", "0.10", " blocks");
    slider("m_attackIntervalMs", "Attack interval", "80", "1000", "10", " ms");

    ConfigNodeV2 restoreNode;
    restoreNode.id = "m_restoreSlot";
    restoreNode.key = "m_restoreSlot";
    restoreNode.title = "Restore previous hotbar slot";
    restoreNode.category = "combat";
    restoreNode.type = ConfigControlTypeV2::Toggle;
    schema.node(std::move(restoreNode));

    ConfigNodeV2 keybindNode;
    keybindNode.id = "keybind";
    keybindNode.key = "keybind";
    keybindNode.title = "Keybind";
    keybindNode.category = "combat";
    keybindNode.type = ConfigControlTypeV2::Keybind;
    schema.node(std::move(keybindNode));
    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

bool MaceSwapperModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "m_enableMaceSwapper" || key == "m_restoreSlot") {
        const bool flag = value == "true" || value == "1" || value == "on";
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (key == "m_enableMaceSwapper") m_enableMaceSwapper = flag;
        else m_restoreSlot = flag;
        return true;
    }
    try {
        std::size_t parsedLength = 0;
        const std::string text(value);
        const float number = std::stof(text, &parsedLength);
        while (parsedLength < text.size() &&
               std::isspace(static_cast<unsigned char>(text[parsedLength]))) ++parsedLength;
        if (parsedLength != text.size() || !std::isfinite(number)) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (key == "m_attackRange") m_attackRange = std::clamp(number, 2.0f, 6.0f);
        else if (key == "m_minimumFallDistance")
            m_minimumFallDistance = bedaim::macefall::normalizeMinimumFallDistance(number);
        else if (key == "m_attackIntervalMs") m_attackIntervalMs = std::clamp(number, 80.0f, 1000.0f);
        else return false;
        return true;
    } catch (...) {
        return false;
    }
}
