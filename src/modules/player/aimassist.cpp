#include "aimassist.hpp"
#include "AimAssistMath.hpp"
#include "AimAssistPolicy.hpp"
#include "../../core/Diagnostics.hpp"
#include "core/InventoryAccess.hpp"
#include "core/GameHooks.hpp"
#include "../ModuleRegistry.hpp"
#include "../hud/gamemodechecker.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/events/LocalPlayerPreTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <dlfcn.h>
#include <cstring>
#include <cctype>
#include <string>
#include <optional>
#include <limits>
#include <atomic>
#include <string_view>
#include <unordered_set>
#include <pl/Logger.hpp>
#include <pl/ModMenu.hpp>


namespace {
using ActorIsPlayerFn = bool (*)(void*);
using ActorIsInvisibleFn = bool (*)(void*);
using ActorGetNameTagFn = const std::string& (*)(void*);
using BlockSourceIsSolidBlockingBlockFn = bool (*)(void*, const bedrocktools::sdk::BlockPos*);
using LocalPlayerApplyTurnDeltaFn = void (*)(void*, bedrocktools::sdk::Vec2*);

struct DistanceSortedActor {
    void* actor;
    float distance;
    float padding;
};

struct ActorVector {
    DistanceSortedActor* begin;
    DistanceSortedActor* end;
    DistanceSortedActor* capacity;
};

using ActorFetchNearbyActorsSortedFn = ActorVector (*)(void*, void*, int);

AimAssistModule* g_aimAssist = nullptr;
ActorIsPlayerFn g_actorIsPlayer = nullptr;
ActorIsInvisibleFn g_actorIsInvisible = nullptr;
ActorGetNameTagFn g_actorGetNameTag = nullptr;
LocalPlayerApplyTurnDeltaFn g_applyLocalPlayerTurnDelta = nullptr;
ActorFetchNearbyActorsSortedFn g_fetchNearbyActors = nullptr;
BlockSourceIsSolidBlockingBlockFn g_isSolidBlockingBlock = nullptr;
HitboxExpanderModule* g_hitboxExpander = nullptr;
std::once_flag g_hitboxExpanderEventOnce;

bool collectNearbyActors(void* origin, float radius, std::unordered_set<void*>& out) {
    if (!origin || !g_fetchNearbyActors || !std::isfinite(radius) || radius <= 0.0f) return false;
    bedrocktools::sdk::Vec3 extent{radius, radius, radius};
    ActorVector actors = g_fetchNearbyActors(origin, &extent, 1);
    const auto begin = reinterpret_cast<std::uintptr_t>(actors.begin);
    const auto end = reinterpret_cast<std::uintptr_t>(actors.end);
    if (!begin || !end || end < begin ||
        (end - begin) % sizeof(DistanceSortedActor) != 0 ||
        (end - begin) / sizeof(DistanceSortedActor) > 16384) return false;
    for (auto* entry = actors.begin; entry < actors.end; ++entry) {
        if (entry->actor && reinterpret_cast<std::uintptr_t>(entry->actor) > 0x1000)
            out.insert(entry->actor);
    }
    return true;
}
std::atomic_bool g_loggedGameTypeUnavailable{false};

pl::log::Logger& aimAssistLogger() {
    static auto& logger = pl::log::Logger::getOrCreate("Bedaim-AimAssist");
    return logger;
}

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;

float wrapDegrees(float angle) {
    return bedaim::aimmath::wrapDegrees(angle);
}

bool finiteVec3(const bedrocktools::sdk::Vec3& v) {
    return bedaim::aimmath::isFiniteVector({v.x, v.y, v.z});
}

bool actorIsGliding(void* actor) {
    using namespace bedrocktools::sdk;
    if (!actor) return false;

    const auto actorAddress = reinterpret_cast<std::uintptr_t>(actor);
    void* dataWrapper = reinterpret_cast<void*>(actorAddress + offsets::Actor::mEntityData);
    if (!dataWrapper) return false;

    void* component = *reinterpret_cast<void**>(dataWrapper);
    if (!component) return false;

    auto** begin = *reinterpret_cast<void***>(component);
    auto** end = *reinterpret_cast<void***>(reinterpret_cast<std::uintptr_t>(component) + sizeof(void*));
    if (!begin || !end) return false;

    const auto beginAddress = reinterpret_cast<std::uintptr_t>(begin);
    const auto endAddress = reinterpret_cast<std::uintptr_t>(end);
    if (endAddress < beginAddress ||
        (endAddress - beginAddress) % sizeof(void*) != 0 ||
        (endAddress - beginAddress) / sizeof(void*) > 192) return false;

    // Synchronized data items are not guaranteed to keep the flags item at index 0.
    // Search the bounded vector for the Flags item (type 7, id 0) instead of treating
    // an unrelated first item as proof that the player is not gliding.
    const auto itemCount = (endAddress - beginAddress) / sizeof(void*);
    for (std::size_t index = 0; index < itemCount; ++index) {
        void* item = begin[index];
        if (!item) continue;

        const auto itemAddress = reinterpret_cast<std::uintptr_t>(item);
        if (itemAddress <= 0x1000) continue;
        const auto type = *reinterpret_cast<const std::uint8_t*>(itemAddress + offsets::DataItem::mType);
        const auto id = *reinterpret_cast<const std::uint16_t*>(itemAddress + offsets::DataItem::mId);
        if (type != 7 || id != 0) continue;

        std::uint64_t flags = 0;
        std::memcpy(&flags, reinterpret_cast<const void*>(itemAddress + offsets::DataItem::mValue),
                    sizeof(flags));
        constexpr std::uint64_t kGlidingFlag = std::uint64_t{1} << 32;
        return (flags & kGlidingFlag) != 0;
    }
    return false;
}

bool validBounds(const bedrocktools::sdk::AABB& bounds) {
    return bedaim::aimmath::isValidBounds({
        {bounds.min.x, bounds.min.y, bounds.min.z},
        {bounds.max.x, bounds.max.y, bounds.max.z}
    });
}

float distanceSquared(const bedrocktools::sdk::Vec3& a, const bedrocktools::sdk::Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bool rayIntersectsAABB(const bedrocktools::sdk::Vec3& origin,
                       const bedrocktools::sdk::Vec3& direction,
                       const bedrocktools::sdk::AABB& bounds) {
    float tMin = 0.0f;
    float tMax = std::numeric_limits<float>::infinity();

    const auto testAxis = [&tMin, &tMax](float rayOrigin, float rayDirection,
                                         float minimum, float maximum) {
        if (std::abs(rayDirection) < 1.0e-6f) {
            return rayOrigin >= minimum && rayOrigin <= maximum;
        }

        float t1 = (minimum - rayOrigin) / rayDirection;
        float t2 = (maximum - rayOrigin) / rayDirection;
        if (t1 > t2) std::swap(t1, t2);

        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        return tMax >= tMin;
    };

    return testAxis(origin.x, direction.x, bounds.min.x, bounds.max.x) &&
           testAxis(origin.y, direction.y, bounds.min.y, bounds.max.y) &&
           testAxis(origin.z, direction.z, bounds.min.z, bounds.max.z) &&
           tMax >= 0.0f;
}

bool hasClearLineOfSight(void* blockSource,
                         const bedrocktools::sdk::Vec3& from,
                         const bedrocktools::sdk::Vec3& to) {
    if (!blockSource || !g_isSolidBlockingBlock || !finiteVec3(from) || !finiteVec3(to)) return false;

    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;
    int x = static_cast<int>(std::floor(from.x));
    int y = static_cast<int>(std::floor(from.y));
    int z = static_cast<int>(std::floor(from.z));
    const int targetX = static_cast<int>(std::floor(to.x));
    const int targetY = static_cast<int>(std::floor(to.y));
    const int targetZ = static_cast<int>(std::floor(to.z));

    const int stepX = (dx > 0.0f) - (dx < 0.0f);
    const int stepY = (dy > 0.0f) - (dy < 0.0f);
    const int stepZ = (dz > 0.0f) - (dz < 0.0f);
    const float inf = std::numeric_limits<float>::infinity();
    const float deltaX = stepX == 0 ? inf : std::abs(1.0f / dx);
    const float deltaY = stepY == 0 ? inf : std::abs(1.0f / dy);
    const float deltaZ = stepZ == 0 ? inf : std::abs(1.0f / dz);
    float maxX = stepX > 0 ? (std::floor(from.x) + 1.0f - from.x) * deltaX
                          : stepX < 0 ? (from.x - std::floor(from.x)) * deltaX : inf;
    float maxY = stepY > 0 ? (std::floor(from.y) + 1.0f - from.y) * deltaY
                          : stepY < 0 ? (from.y - std::floor(from.y)) * deltaY : inf;
    float maxZ = stepZ > 0 ? (std::floor(from.z) + 1.0f - from.z) * deltaZ
                          : stepZ < 0 ? (from.z - std::floor(from.z)) * deltaZ : inf;

    const int maxSteps = std::min(1024,
        std::abs(targetX - x) + std::abs(targetY - y) + std::abs(targetZ - z) + 4);
    for (int i = 0; i < maxSteps; ++i) {
        if (x == targetX && y == targetY && z == targetZ) return true;

        if (maxX <= maxY && maxX <= maxZ) {
            x += stepX;
            maxX += deltaX;
        } else if (maxY <= maxZ) {
            y += stepY;
            maxY += deltaY;
        } else {
            z += stepZ;
            maxZ += deltaZ;
        }

        // Do not test the target's own voxel; the endpoint may lie inside its hitbox.
        if (x == targetX && y == targetY && z == targetZ) return true;
        const bedrocktools::sdk::BlockPos cell{x, y, z};
        if (g_isSolidBlockingBlock(blockSource, &cell)) return false;
    }
    return x == targetX && y == targetY && z == targetZ;
}



bool categoryHasMobBit(void* actor) {
    if (!actor) return false;
    const auto categories = *reinterpret_cast<const std::uint32_t*>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mCategories);
    // BedrockTools' existing Hitbox module uses category bit 2 to identify entities.
    return (categories & 2u) != 0;
}

std::string normalizeName(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (i + 2 < value.size() && byte == 0xC2u &&
            static_cast<unsigned char>(value[i + 1]) == 0xA7u) {
            i += 2;
            continue;
        }
        result.push_back(static_cast<char>(std::tolower(byte)));
    }
    const auto first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = result.find_last_not_of(" \t\r\n");
    return result.substr(first, last - first + 1);
}

bool whitelistContainsName(const std::string& whitelist, const std::string& name) {
    const std::string normalizedName = normalizeName(name);
    if (normalizedName.empty()) return false;
    std::size_t start = 0;
    while (start <= whitelist.size()) {
        const std::size_t end = whitelist.find_first_of(",;\r\n", start);
        const std::string entry = normalizeName(whitelist.substr(
            start, end == std::string::npos ? std::string::npos : end - start));
        if (!entry.empty() && entry == normalizedName) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

bool isWhitelistedActor(void* actor, bool isPlayer, const std::string& whitelist) {
    // This list is intended to ignore player names (including bot accounts).
    // Do not call the resolved native Actor::getNameTag function here: it is
    // unnecessary for player filtering and has crashed on some actor instances
    // when a non-empty list caused this path to run.
    if (!actor || !isPlayer || normalizeName(whitelist).empty()) return false;
    const auto* player = reinterpret_cast<bedrocktools::sdk::Player*>(actor);
    return whitelistContainsName(whitelist, player->name());
}

using HeldWeapon = bedaim::aimpolicy::HeldWeapon;

HeldWeapon heldWeaponInMainHand(const bedrocktools::sdk::Player* player) {
    if (!player) return HeldWeapon::None;

    // Use the same selected-hotbar path as the swapper modules. On some
    // BedrockTools/LeviLamina builds Player::getCarriedItem returns a transient
    // wrapper that does not describe the currently selected inventory stack;
    // classifying that wrapper as None made Weapon Filter disable every weapon.
    const std::string identifier =
        bedrocktools::core::InventoryAccess::get().selectedHotbarIdentifier(player);
    if (identifier.empty()) return HeldWeapon::None;
    return bedaim::aimpolicy::classifyHeldWeaponIdentifier(identifier);
}

bool weaponAllowed(HeldWeapon weapon, bool sword, bool axe, bool mace,
                   bool bow, bool crossbow, bool trident, bool spear, bool otherMelee) {
    switch (weapon) {
        case HeldWeapon::Sword: return sword;
        case HeldWeapon::Axe: return axe;
        case HeldWeapon::Mace: return mace;
        case HeldWeapon::Bow: return bow;
        case HeldWeapon::Crossbow: return crossbow;
        case HeldWeapon::Trident: return trident;
        case HeldWeapon::Spear: return spear;
        case HeldWeapon::OtherMelee: return otherMelee;
        case HeldWeapon::Tool: return false;
        case HeldWeapon::None: default: return false;
    }
}

bool naturalizationAppliesToWeapon(const std::string& setting, HeldWeapon weapon) {
    if (setting == "All") return true;
    if (setting == "Sword") return weapon == HeldWeapon::Sword;
    if (setting == "Axe") return weapon == HeldWeapon::Axe;
    if (setting == "Mace") return weapon == HeldWeapon::Mace;
    if (setting == "Bow") return weapon == HeldWeapon::Bow;
    if (setting == "Crossbow") return weapon == HeldWeapon::Crossbow;
    if (setting == "Trident") return weapon == HeldWeapon::Trident;
    if (setting == "Spear") return weapon == HeldWeapon::Spear;
    if (setting == "Other melee") return weapon == HeldWeapon::OtherMelee;
    if (setting == "Tools") return weapon == HeldWeapon::Tool;
    return false;
}

}

AimAssistModule::AimAssistModule()
    : Module("BedrockAim", "Smooth target tracking for private PvP training worlds.", "Aim Assist") {
    keybind = 0;
    g_aimAssist = this;
}

AimAssistModule::~AimAssistModule() {
    if (g_aimAssist == this) g_aimAssist = nullptr;
}

void AimAssistModule::resetTrackingState() {
    m_lastPlayer = nullptr;
    m_lastTarget = nullptr;
    m_targetPresent = false;
    m_lastTargetIsPlayer = false;
    m_cachedHeldWeaponCode = 0;
    m_cachedWeaponSlot = -1;
    m_nextHeldWeaponRefresh = {};
    m_weaponFilterAllowsCurrent = true;
    m_lastTargetBounds = {};
    m_lastAimPoint = {};
    m_hasAimPoint = false;
    m_localPlayerGliding = false;
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    m_pitchHoldUntil = {};
    m_lastObservedRotation = {};
    m_hasObservedRotation = false;
    m_lastAssistYawDelta = 0.0f;
    m_lastAssistPitchDelta = 0.0f;
    m_lastRotationUpdate = {};
    m_lastTargetSeen = {};
    m_motionTrackedTarget = nullptr;
    m_lastMotionPosition = {};
    m_targetVelocity = {};
    m_hasMotionSample = false;
    m_lastMotionSample = {};
    m_lastPredictionPlayerPosition = {};
    m_playerVelocity = {};
    m_hasPredictionPlayerSample = false;
    m_lastPredictionPlayerSample = {};
}

void AimAssistModule::onInit() {
    g_fetchNearbyActors = reinterpret_cast<ActorFetchNearbyActorsSortedFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));
    // GameType lookup is centralized and retried by GameHooks after runtime load.
    g_isSolidBlockingBlock = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock));
    g_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    g_actorIsInvisible = reinterpret_cast<ActorIsInvisibleFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsInvisible));
    g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));
    g_applyLocalPlayerTurnDelta = reinterpret_cast<LocalPlayerApplyTurnDeltaFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LocalPlayerApplyTurnDelta));

    const bool targetQueryReady = g_fetchNearbyActors != nullptr;
    const bool targetClassificationReady = g_actorIsPlayer != nullptr;
    const bool cameraControlReady = g_applyLocalPlayerTurnDelta != nullptr;
    const bool requiredReady = targetQueryReady && targetClassificationReady && cameraControlReady;
    bedrocktools::diagnostics::probe(
        requiredReady ? bedrocktools::diagnostics::Level::Info : bedrocktools::diagnostics::Level::Warning,
        "BedrockAim", "native_dependencies", requiredReady ? "ready" : "partial",
        std::string("nearby_query=") + (targetQueryReady ? "ok" : "missing") +
        ", actor_classification=" + (targetClassificationReady ? "ok" : "missing") +
        ", camera_control=" + (cameraControlReady ? "ok" : "missing") +
        ", visibility=" + (g_isSolidBlockingBlock ? "ok" : "unavailable") +
        ", names=" + (g_actorGetNameTag ? "ok" : "unavailable"));

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_aimAssist && g_aimAssist->enabled) {
                g_aimAssist->processPlayerTick(event.player);
            }
        });
    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>(
        [](auto& event) {
            auto* module = g_aimAssist;
            if (!module) return;
            if (!event.clientInstance) return;
            // Reuse the known-good player pointer from LocalPlayerTickEvent.
            // A second localPlayer() lookup may transiently return null and wipe tracking.
            module->processPlayerFrame();
        });


}

void AimAssistModule::onEnable() {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    resetTrackingState();
}

void AimAssistModule::onDisable() {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    resetTrackingState();
}

void AimAssistModule::onMenuRegistered() {
    if (!m_numericSettingsRegistered.exchange(true, std::memory_order_acq_rel)) {
        using namespace pl::modmenu;
        ModuleBuilder settings("bedaim.aimassist.write_settings", "Aim Assist Write Settings");
        settings.modId("Bedaim")
            .description("Enter exact numeric values for Aim Assist without dragging sliders.")
            .defaultEnabled(true)
            .config("rotationSpeed", "Rotation Speed (exact)", ConfigType::Text, "1.00")
            .config("aimSpeed", "Aim Speed (exact)", ConfigType::Text, "5.00")
            .config("fov", "FOV (exact)", ConfigType::Text, "35.00")
            .config("range", "Aim Range (exact)", ConfigType::Text, "5.50")
            .config("strength", "Strength (exact)", ConfigType::Text, "0.25")
            .config("hitboxScale", "Hitbox Area (exact)", ConfigType::Text, "1.00")
            .config("hitboxDistanceGrowth", "Hitbox Growth (exact)", ConfigType::Text, "5.00")
            .config("predictionLeadMs", "Prediction Lead ms (exact)", ConfigType::Text, "120.00")
            .onConfigChanged([this](std::string_view, std::string_view key, std::string_view value) {
                onMenuConfigChanged(key, value);
            });
        if (!settings.registerModule()) m_numericSettingsRegistered.store(false, std::memory_order_release);
    }
}

bool AimAssistModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    auto parseNumber = [&]() -> std::optional<float> {
        try {
            const std::string text(value);
            std::size_t parsedLength = 0;
            const float number = std::stof(text, &parsedLength);
            while (parsedLength < text.size() &&
                   std::isspace(static_cast<unsigned char>(text[parsedLength]))) ++parsedLength;
            if (parsedLength != text.size() || !std::isfinite(number)) return std::nullopt;
            return number;
        } catch (...) { return std::nullopt; }
    };
    const auto parsed = parseNumber();
    if (!parsed) return false;
    const float number = *parsed;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (key == "m_rotationSpeed" || key == "rotationSpeed") m_rotationSpeed = std::clamp(number, 0.1f, 1000.0f);
    else if (key == "aimSpeed") m_aimSpeed = std::clamp(number, 0.1f, 10.0f);
    else if (key == "fov") m_fov = std::clamp(number, 1.0f, 360.0f);
    else if (key == "range") m_range = std::clamp(number, 2.0f, 80.0f);
    else if (key == "strength") m_strength = std::clamp(number, 0.0f, 1.0f);
    else if (key == "hitboxScale") m_hitboxScale = std::clamp(number, 0.30f, 50.0f);
    else if (key == "hitboxDistanceGrowth") m_hitboxDistanceGrowth = std::clamp(number, 0.1f, 500.0f);
    else if (key == "swordAimPointY" || key == "m_swordAimPointY") m_swordAimPointY = std::clamp(number, 0.05f, 0.95f);
    else if (key == "axeAimPointY" || key == "m_axeAimPointY") m_axeAimPointY = std::clamp(number, 0.05f, 0.95f);
    else if (key == "maceAimPointY" || key == "m_maceAimPointY") m_maceAimPointY = std::clamp(number, 0.05f, 0.95f);
    else if (key == "bowAimPointY" || key == "m_bowAimPointY") m_bowAimPointY = std::clamp(number, 0.05f, 0.95f);
    else if (key == "crossbowAimPointY" || key == "m_crossbowAimPointY") m_crossbowAimPointY = std::clamp(number, 0.05f, 0.95f);
    else if (key == "tridentAimPointY" || key == "m_tridentAimPointY") m_tridentAimPointY = std::clamp(number, 0.05f, 0.95f);
    else if (key == "spearAimPointY" || key == "m_spearAimPointY") m_spearAimPointY = std::clamp(number, 0.05f, 0.95f);
    else if (key == "otherMeleeAimPointY" || key == "m_otherMeleeAimPointY") m_otherMeleeAimPointY = std::clamp(number, 0.05f, 0.95f);
    else if (key == "fourPointAiming" || key == "m_fourPointAiming") m_fourPointAiming = value == "true" || value == "1" || value == "on";
    else if (key == "predictionLeadMs") m_predictionLeadMs = std::clamp(number, 0.0f, 300.0f);
    else return false;
    return true;
}






void AimAssistModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    processPlayerTickLocked(player);
}

bool AimAssistModule::currentTargetSnapshot(void*& target, bedrocktools::sdk::AABB& bounds, float& range) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    target = nullptr;
    bounds = {};
    range = std::clamp(m_range, 2.0f, 80.0f);
    const auto now = std::chrono::steady_clock::now();
    if (!enabled || !m_lastPlayer || !m_lastTarget || !m_targetPresent ||
        !validBounds(m_lastTargetBounds) ||
        m_lastTargetSeen.time_since_epoch().count() == 0 ||
        now < m_lastTargetSeen ||
        now - m_lastTargetSeen > std::chrono::milliseconds(250)) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "BedrockAim", "target_snapshot", "stale_or_missing");
        return false;
    }
    target = m_lastTarget;
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
        "BedrockAim", "target_snapshot", "fresh");
    bounds = m_lastTargetBounds;
    return true;
}

bool AimAssistModule::currentTargetIsPlayerSnapshot(void* desiredTarget) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    const auto now = std::chrono::steady_clock::now();
    return enabled && desiredTarget && desiredTarget == m_lastTarget &&
        m_targetPresent && m_lastTargetIsPlayer && validBounds(m_lastTargetBounds) &&
        m_lastTargetSeen.time_since_epoch().count() != 0 &&
        now >= m_lastTargetSeen &&
        now - m_lastTargetSeen <= std::chrono::milliseconds(250);
}

bool AimAssistModule::targetBoundsSnapshot(void* desiredTarget,
                                                   bedrocktools::sdk::AABB& bounds,
                                                   float& range) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    bounds = {};
    range = std::clamp(m_range, 2.0f, 80.0f);
    auto* player = m_lastPlayer;
    if (!enabled || !player || !desiredTarget || !g_fetchNearbyActors) return false;

    // Resolve the desired actor against a fresh engine-owned actor snapshot before
    // dereferencing it. This lets short multi-tick actions (such as a mace swap)
    // keep their original target without trusting a cached raw pointer.
    Vec3 extent{range, range, range};
    ActorVector actors = g_fetchNearbyActors(player, &extent, 1);
    const auto beginAddress = reinterpret_cast<std::uintptr_t>(actors.begin);
    const auto endAddress = reinterpret_cast<std::uintptr_t>(actors.end);
    if (!actors.begin || !actors.end || endAddress < beginAddress ||
        (endAddress - beginAddress) % sizeof(DistanceSortedActor) != 0 ||
        (endAddress - beginAddress) / sizeof(DistanceSortedActor) > 4096) return false;

    for (auto* entry = actors.begin; entry < actors.end; ++entry) {
        if (entry->actor != desiredTarget) continue;
        auto* actor = reinterpret_cast<Actor*>(desiredTarget);
        const Vec3 position = actor->position();
        const AABB candidateBounds = actor->bounds();
        if (!finiteVec3(position) || !validBounds(candidateBounds)) return false;
        bounds = candidateBounds;
        return true;
    }
    return false;
}

bool AimAssistModule::currentTargetInCrosshair(bedrocktools::sdk::Player* player, void* target, float tolerance) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    const auto now = std::chrono::steady_clock::now();
    if (!enabled || !player || player != m_lastPlayer || !target ||
        target != m_lastTarget || !m_targetPresent || !validBounds(m_lastTargetBounds) ||
        m_lastTargetSeen.time_since_epoch().count() == 0 ||
        now < m_lastTargetSeen ||
        now - m_lastTargetSeen > std::chrono::milliseconds(250)) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "BedrockAim", "crosshair_check", "stale_or_invalid_target");
        return false;
    }

    auto* rotationComponent = player->rotationComponent();
    if (!rotationComponent) return false;
    Vec2 rotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y)) return false;
    rotation.x = std::clamp(rotation.x, -89.0f, 89.0f);
    rotation.y = wrapDegrees(rotation.y);

    const Vec3 playerPosition = player->position();
    const AABB playerBounds = player->bounds();
    if (!finiteVec3(playerPosition)) return false;
    Vec3 eyePosition = playerPosition;
    if (validBounds(playerBounds)) {
        eyePosition.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        eyePosition.y = playerBounds.min.y + (playerBounds.max.y - playerBounds.min.y) * 0.85f;
        eyePosition.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }

    const float yaw = rotation.y / kRadToDeg;
    const float pitch = rotation.x / kRadToDeg;
    const float cosPitch = std::cos(pitch);
    const Vec3 viewDirection{
        -std::sin(yaw) * cosPitch,
        -std::sin(pitch),
        std::cos(yaw) * cosPitch
    };
    // Small tolerance handles mobile touch quantization. When enabled, the
    // dedicated expander participates in this exact same target-ray test.
    const float crosshairTolerance = std::clamp(tolerance, 0.05f, 0.8f);
    AABB targetQueryBounds = m_lastTargetBounds;
    auto* expanderBase = ModuleRegistry::get().find("bedrocktools.Hitbox Expander");
    auto* expander = (expanderBase && expanderBase->enabled)
        ? static_cast<HitboxExpanderModule*>(expanderBase) : nullptr;
    if (expander && g_actorIsPlayer) {
        targetQueryBounds = expander->expandedTargetBounds(g_actorIsPlayer(target), m_lastTargetBounds);
    }
    const AABB crosshairBounds{
        {targetQueryBounds.min.x - crosshairTolerance,
         targetQueryBounds.min.y - crosshairTolerance,
         targetQueryBounds.min.z - crosshairTolerance},
        {targetQueryBounds.max.x + crosshairTolerance,
         targetQueryBounds.max.y + crosshairTolerance,
         targetQueryBounds.max.z + crosshairTolerance}
    };
    if (!rayIntersectsAABB(eyePosition, viewDirection, crosshairBounds)) return false;

    if (m_cantSeeThroughBlocks || (expander && expander->requiresLineOfSight())) {
        auto* dimension = player->dimension();
        void* blockSource = dimension ? static_cast<void*>(dimension->blockSource()) : nullptr;
        const Vec3 center{
            (m_lastTargetBounds.min.x + m_lastTargetBounds.max.x) * 0.5f,
            (m_lastTargetBounds.min.y + m_lastTargetBounds.max.y) * 0.5f,
            (m_lastTargetBounds.min.z + m_lastTargetBounds.max.z) * 0.5f
        };
        if (blockSource && g_isSolidBlockingBlock &&
            !hasClearLineOfSight(blockSource, eyePosition, center)) return false;
    }
    return true;
}

void AimAssistModule::processPlayerTickLocked(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;

    if (!enabled || !player || !g_fetchNearbyActors || (!m_targetPlayers && !m_targetMobs)) {
        const char* reason = !enabled ? "disabled" : !player ? "missing_local_player" :
            !g_fetchNearbyActors ? "nearby_query_unavailable" : "all_target_categories_disabled";
        bedrocktools::diagnostics::probe(
            !player || !g_fetchNearbyActors ? bedrocktools::diagnostics::Level::Warning :
                bedrocktools::diagnostics::Level::Debug,
            "BedrockAim", "tracking_gate", reason);
        resetTrackingState();
        return;
    }
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
        "BedrockAim", "tracking_gate", "ready");

    m_lastPlayer = player;
    m_localPlayerGliding = actorIsGliding(player);

    // Native ItemStack description lookup is relatively expensive. Reclassify
    // immediately on hotbar changes and otherwise at 10 Hz; weapon toggles still
    // apply every tick to the cached category, without repeating native lookups.
    auto& inventory = bedrocktools::core::InventoryAccess::get();
    const int selectedWeaponSlot = inventory.selectedHotbarSlot(player);
    const auto weaponCheckTime = std::chrono::steady_clock::now();
    if (selectedWeaponSlot != m_cachedWeaponSlot ||
        m_nextHeldWeaponRefresh.time_since_epoch().count() == 0 ||
        weaponCheckTime >= m_nextHeldWeaponRefresh) {
        m_cachedHeldWeaponCode = static_cast<int>(heldWeaponInMainHand(player));
        m_cachedWeaponSlot = selectedWeaponSlot;
        m_nextHeldWeaponRefresh = weaponCheckTime + std::chrono::milliseconds(100);
    }
    const HeldWeapon heldWeapon = static_cast<HeldWeapon>(m_cachedHeldWeaponCode);
    m_weaponFilterAllowsCurrent = !m_weaponOnly ||
        weaponAllowed(heldWeapon, m_weaponSword, m_weaponAxe, m_weaponMace,
            m_weaponBow, m_weaponCrossbow, m_weaponTrident, m_weaponSpear, m_weaponOtherMelee);
    // Keep target geometry fresh for Sword/Mace Swapper even when the currently
    // held weapon is filtered out. Camera movement and automatic attack still respect the filter.

    auto* rotationComponent = player->rotationComponent();
    if (!rotationComponent) {
        resetTrackingState();
        return;
    }

    auto* expanderBase = ModuleRegistry::get().find("bedrocktools.Hitbox Expander");
    auto* expander = (expanderBase && expanderBase->enabled)
        ? static_cast<HitboxExpanderModule*>(expanderBase) : nullptr;
    const bool expanderRequiresLineOfSight = expander && expander->requiresLineOfSight();

    void* blockSource = nullptr;
    if (m_cantSeeThroughBlocks || expanderRequiresLineOfSight) {
        auto* dimension = player->dimension();
        blockSource = dimension ? static_cast<void*>(dimension->blockSource()) : nullptr;
    }

    Vec2 currentRotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(currentRotation.x) || !std::isfinite(currentRotation.y)) {
        resetTrackingState();
        return;
    }
    currentRotation.x = std::clamp(currentRotation.x, -89.0f, 89.0f);
    currentRotation.y = wrapDegrees(currentRotation.y);

    const AABB playerBounds = player->bounds();
    const Vec3 playerPosition = player->position();
    if (!finiteVec3(playerPosition)) {
        resetTrackingState();
        return;
    }

    // Detect the start of an upward jump from consecutive local-player positions.
    // Hold pitch steady briefly so the assist can keep tracking horizontally
    // without pulling the camera toward the target's lower/central hitbox mid-jump.
    const bool naturalizationForWeapon =
        m_naturalization && naturalizationAppliesToWeapon(m_naturalizationWeapon, heldWeapon);
    if (naturalizationForWeapon) {
        if (m_hasLastPlayerY) {
            const float verticalDelta = playerPosition.y - m_lastPlayerY;
            if (std::isfinite(verticalDelta) && verticalDelta > 0.035f &&
                verticalDelta < 0.85f) {
                m_pitchHoldUntil = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(550);
            }
        }
        m_lastPlayerY = playerPosition.y;
        m_hasLastPlayerY = true;
    } else {
        m_lastPlayerY = playerPosition.y;
        m_hasLastPlayerY = true;
        m_pitchHoldUntil = {};
    }

    Vec3 eyePosition = playerPosition;
    if (validBounds(playerBounds)) {
        eyePosition.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        eyePosition.y = playerBounds.min.y + (playerBounds.max.y - playerBounds.min.y) * 0.85f;
        eyePosition.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }

    const float range = std::clamp(m_range, 2.0f, 80.0f);
    const float expansionMargin = expander ? expander->queryExpansionMargin() : 0.0f;
    const float scanRange = std::clamp(range + expansionMargin, 2.0f, 80.0f);
    Vec3 extent{scanRange, scanRange, scanRange};
    // The native nearby-actors function returns the actual candidate snapshot;
    // don't gate it on Level::mActorManager, an unused offset that can vary by build.
    ActorVector actors = g_fetchNearbyActors(player, &extent, 1);
    const auto actorsBeginAddress = reinterpret_cast<std::uintptr_t>(actors.begin);
    const auto actorsEndAddress = reinterpret_cast<std::uintptr_t>(actors.end);
    if (!actors.begin || !actors.end || actorsEndAddress < actorsBeginAddress ||
        (actorsEndAddress - actorsBeginAddress) % sizeof(DistanceSortedActor) != 0 ||
        (actorsEndAddress - actorsBeginAddress) / sizeof(DistanceSortedActor) > 4096) {
        // Reject malformed native vector metadata before iterating it.
        resetTrackingState();
        return;
    }

    void* bestTarget = nullptr;
    AABB bestTargetBounds{};
    bool bestTargetIsPlayer = false;
    float bestScore = std::numeric_limits<float>::max();
    void* retainedTarget = nullptr;
    AABB retainedTargetBounds{};
    float retainedTargetScore = std::numeric_limits<float>::max();
    bool retainedTargetInsideBubble = false;
    bool retainedTargetIsPlayer = false;
    Vec3 playerCenter = playerPosition;
    if (validBounds(playerBounds)) {
        playerCenter.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        playerCenter.y = (playerBounds.min.y + playerBounds.max.y) * 0.5f;
        playerCenter.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    }
    const float retentionRadius = m_localPlayerGliding
        ? 0.0f
        : std::clamp(m_targetRetentionRadius, 0.5f, 5.0f);
    const float retentionRadiusSq = retentionRadius * retentionRadius;
    const float maxDistanceSq = range * range;
    // Experimental full-range FOV. Range, entity type, visibility, and valid bounds
    // remain required even when the angular limit is set to 360 degrees.
    const float maxFov = std::clamp(m_fov, 1.0f, 360.0f);
    const float yawRadians = currentRotation.y / kRadToDeg;
    const float pitchRadians = currentRotation.x / kRadToDeg;
    const float cosPitch = std::cos(pitchRadians);
    const Vec3 viewDirection{
        -std::sin(yawRadians) * cosPitch,
        -std::sin(pitchRadians),
        std::cos(yawRadians) * cosPitch
    };

    for (auto* entry = actors.begin; entry < actors.end; ++entry) {
        void* candidate = entry->actor;
        if (!candidate || candidate == player) continue;

        const bool isPlayer = g_actorIsPlayer && g_actorIsPlayer(candidate);
        if (isPlayer) {
            if (!m_targetPlayers) continue;
            if (m_ignoreCreativeSpectator) {
                std::int32_t gameType = -1;
                const bool hasExactGameType =
                    bedrocktools::core::gamehooks::tryGetPlayerGameType(candidate, gameType);
                if (hasExactGameType && (gameType == 1 || gameType == 3)) continue;

                std::int32_t predicateMode = -1;
                const bool hasExactModePredicate = !hasExactGameType &&
                    bedrocktools::core::gamehooks::tryGetPlayerCreativeSpectator(candidate, predicateMode);
                if (hasExactModePredicate && (predicateMode == 1 || predicateMode == 3)) continue;

                if (!hasExactGameType && !hasExactModePredicate &&
                    bedrocktools::core::gamehooks::tryGetPlayerNoDamageCreativeHint(candidate)) {
                    // Repeated hit attempts with no observed damage are only a Creative candidate.
                    continue;
                }
                if (!hasExactGameType && !hasExactModePredicate &&
                    GamemodeCheckerModule::isSpectatorCandidate(candidate)) {
                    continue;
                }

                bool isAttackable = true;
                if (bedrocktools::core::gamehooks::tryIsActorAttackableGamemode(
                        candidate, isAttackable)) {
                    if (!isAttackable) continue;
                } else if (!hasExactGameType && !hasExactModePredicate &&
                           !g_loggedGameTypeUnavailable.exchange(true, std::memory_order_acq_rel)) {
                    aimAssistLogger().warn(
                        "Creative/Spectator filter has no exact native signal; no-damage attacks are tracked heuristically.");
                }
            }
        } else {
            if (!m_targetMobs || !categoryHasMobBit(candidate)) continue;
        }

        if (isWhitelistedActor(candidate, isPlayer, m_whitelistNames)) continue;
        if (g_actorIsInvisible && g_actorIsInvisible(candidate)) continue;

        auto* target = reinterpret_cast<Actor*>(candidate);
        const Vec3 targetPosition = target->position();
        const AABB targetBounds = target->bounds();
        if (!finiteVec3(targetPosition) || !validBounds(targetBounds)) continue;

        // Aim at a stable point on the native bounds but allow the bounded
        // expanded client box to participate in target acquisition and automatic attack range.
        const AABB selectionBounds = expander
            ? expander->expandedTargetBounds(isPlayer, targetBounds) : targetBounds;
        const Vec3 centerAimPoint{
            (targetBounds.min.x + targetBounds.max.x) * 0.5f,
            targetBounds.min.y + (targetBounds.max.y - targetBounds.min.y) * 0.55f,
            (targetBounds.min.z + targetBounds.max.z) * 0.5f
        };
        const Vec3 nearestOnSelection{
            std::clamp(eyePosition.x, selectionBounds.min.x, selectionBounds.max.x),
            std::clamp(eyePosition.y, selectionBounds.min.y, selectionBounds.max.y),
            std::clamp(eyePosition.z, selectionBounds.min.z, selectionBounds.max.z)
        };
        const float selectionDistanceSq = distanceSquared(eyePosition, nearestOnSelection);

        // Fail closed when the visibility signature or block source is unavailable.
        // hasClearLineOfSight() returns false if it cannot safely perform the check.
        // If the LOS signature is unavailable for this Minecraft build, don't
        // silently discard every candidate. Enforce LOS only when the checker is live.
        if ((m_cantSeeThroughBlocks || expanderRequiresLineOfSight) && blockSource &&
            g_isSolidBlockingBlock && !hasClearLineOfSight(blockSource, eyePosition, centerAimPoint)) continue;

        const float d2 = distanceSquared(eyePosition, centerAimPoint);
        // Range checks use the nearest point on the selected hitbox geometry, not
        // its center; the box itself still cannot exceed the configured size limits.
        const bool inAimAssistRange = std::isfinite(selectionDistanceSq) &&
            selectionDistanceSq <= maxDistanceSq;
        if (!std::isfinite(d2) || !inAimAssistRange) continue;

        // If the current target is right beside/around the player, keep it locked
        // while it crosses behind the camera. Measure distance to the target AABB,
        // not its center, so large hitboxes don't cause a sudden lock drop.
        bool insideRetentionBubble = false;
        if (candidate == m_lastTarget) {
            const Vec3 nearestOnTarget{
                std::clamp(playerCenter.x, targetBounds.min.x, targetBounds.max.x),
                std::clamp(playerCenter.y, targetBounds.min.y, targetBounds.max.y),
                std::clamp(playerCenter.z, targetBounds.min.z, targetBounds.max.z)
            };
            insideRetentionBubble = !m_localPlayerGliding &&
                distanceSquared(playerCenter, nearestOnTarget) <= retentionRadiusSq;
        }

        const float dx = centerAimPoint.x - eyePosition.x;
        const float dy = centerAimPoint.y - eyePosition.y;
        const float dz = centerAimPoint.z - eyePosition.z;
        const float horizontal = std::sqrt(dx * dx + dz * dz);
        const float desiredYaw = std::atan2(-dx, dz) * kRadToDeg;
        const float desiredPitch = -std::atan2(dy, std::max(horizontal, 0.001f)) * kRadToDeg;

        const float targetDistance = std::sqrt(std::max(d2, 0.0f));
        if (!std::isfinite(targetDistance)) continue;

        const float yawError = wrapDegrees(desiredYaw - currentRotation.y);
        const float pitchError = desiredPitch - currentRotation.x;
        const float angleScore = std::sqrt(yawError * yawError + pitchError * pitchError);
        if (!std::isfinite(angleScore)) continue;

        // Keep targets directly below the player's feet eligible during a mace
        // slam, and permit a precise ray hit through the configured expanded box.
        const float playerFeetY = validBounds(playerBounds) ? playerBounds.min.y : playerPosition.y;
        const bool candidateBelowPlayer = !m_localPlayerGliding &&
            targetBounds.max.y <= playerFeetY + 0.12f;
        const bool rayHitsExpandedTarget = expander &&
            rayIntersectsAABB(eyePosition, viewDirection, selectionBounds);

        // The last target also gets a small 3D retention bubble, including when it
        // passes below the player or behind the camera. This prevents target loss
        // during the fast vertical movement of a slam.
        if (angleScore > maxFov && !insideRetentionBubble && !candidateBelowPlayer &&
            !rayHitsExpandedTarget) continue;

        // A ray through the expanded box is a high-confidence crosshair match,
        // not an angle-only candidate that happens to fall inside the FOV.
        const bool playerDistanceMode = (m_targetSelectionMode == "Player");
        const float selectionAngleScore = rayHitsExpandedTarget ? 0.0f : angleScore;
        const float score = playerDistanceMode
            ? targetDistance
            : selectionAngleScore + targetDistance * 0.0001f;
        if (candidate == m_lastTarget) {
            retainedTarget = candidate;
            retainedTargetBounds = targetBounds;
            retainedTargetIsPlayer = isPlayer;
            retainedTargetScore = score;
            retainedTargetInsideBubble = insideRetentionBubble || candidateBelowPlayer;
        }
        if (score < bestScore) {
            bestScore = score;
            bestTarget = candidate;
            bestTargetBounds = targetBounds;
            bestTargetIsPlayer = isPlayer;
        }
    }

    // Prefer the current target when a challenger is only marginally better.
    // Angular mode uses degrees; player-distance mode uses blocks, so their
    // margins stay independent and physically meaningful.
    const bool playerDistanceMode = (m_targetSelectionMode == "Player");
    const float switchMargin = playerDistanceMode
        ? std::clamp(m_targetSwitchDistanceMargin, 0.0f, 5.0f)
        : std::clamp(m_targetSwitchHysteresis, 0.0f, 12.0f);
    if (retainedTarget && (retainedTargetInsideBubble || retainedTargetScore <= bestScore + switchMargin)) {
        bestTarget = retainedTarget;
        bestTargetBounds = retainedTargetBounds;
        bestTargetIsPlayer = retainedTargetIsPlayer;
    }

    if (!bestTarget) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "BedrockAim", "target_selection", "none",
            "no eligible actor in the scan radius passed the current filters");
        // Keep the last target briefly while it crosses the player's immediate
        // area or the native nearby-actor list skips one tick. Do not dereference
        // the cached entity or auto-hit it while it is missing from the list.
        const auto now = std::chrono::steady_clock::now();
        if (m_lastTarget && validBounds(m_lastTargetBounds) &&
            m_lastTargetSeen.time_since_epoch().count() != 0 &&
            now - m_lastTargetSeen <= std::chrono::milliseconds(250)) {
            const Vec3 nearestOnPreviousTarget{
                std::clamp(playerCenter.x, m_lastTargetBounds.min.x, m_lastTargetBounds.max.x),
                std::clamp(playerCenter.y, m_lastTargetBounds.min.y, m_lastTargetBounds.max.y),
                std::clamp(playerCenter.z, m_lastTargetBounds.min.z, m_lastTargetBounds.max.z)
            };
            if (distanceSquared(playerCenter, nearestOnPreviousTarget) <= retentionRadiusSq) {
                m_targetPresent = false;
                return;
            }
        }
        resetTrackingState();
        return;
    }

    const bool targetChanged = bestTarget != m_lastTarget;
    const auto motionSampleTime = std::chrono::steady_clock::now();

    // Sample our own motion as well as the target. The prediction code below
    // subtracts local-player velocity from target velocity; without this sample
    // the local term stayed zero forever, so moving toward a stationary target
    // never influenced the predicted relative aim point.
    if (m_hasPredictionPlayerSample && finiteVec3(playerPosition)) {
        const float playerDt = std::chrono::duration<float>(
            motionSampleTime - m_lastPredictionPlayerSample).count();
        if (std::isfinite(playerDt) && playerDt >= 0.01f && playerDt <= 0.25f) {
            const Vec3 measuredVelocity{
                (playerPosition.x - m_lastPredictionPlayerPosition.x) / playerDt,
                (playerPosition.y - m_lastPredictionPlayerPosition.y) / playerDt,
                (playerPosition.z - m_lastPredictionPlayerPosition.z) / playerDt
            };
            const float playerSpeedSq = distanceSquared(measuredVelocity, Vec3{});
            if (std::isfinite(playerSpeedSq) && playerSpeedSq <= 225.0f) {
                m_playerVelocity.x = m_playerVelocity.x * 0.35f + measuredVelocity.x * 0.65f;
                m_playerVelocity.y = m_playerVelocity.y * 0.35f + measuredVelocity.y * 0.65f;
                m_playerVelocity.z = m_playerVelocity.z * 0.35f + measuredVelocity.z * 0.65f;
            } else {
                m_playerVelocity = {};
            }
        } else {
            m_playerVelocity = {};
        }
    } else {
        m_playerVelocity = {};
    }
    m_lastPredictionPlayerPosition = finiteVec3(playerPosition) ? playerPosition : Vec3{};
    m_hasPredictionPlayerSample = finiteVec3(playerPosition);
    m_lastPredictionPlayerSample = motionSampleTime;

    const Vec3 targetPosition = reinterpret_cast<Actor*>(bestTarget)->position();
    if (targetChanged || m_motionTrackedTarget != bestTarget || !m_hasMotionSample ||
        !finiteVec3(targetPosition)) {
        m_targetVelocity = {};
    } else {
        const float dt = std::chrono::duration<float>(motionSampleTime - m_lastMotionSample).count();
        if (std::isfinite(dt) && dt >= 0.01f && dt <= 0.25f) {
            Vec3 velocity{
                (targetPosition.x - m_lastMotionPosition.x) / dt,
                (targetPosition.y - m_lastMotionPosition.y) / dt,
                (targetPosition.z - m_lastMotionPosition.z) / dt
            };
            const float speedSq = distanceSquared(velocity, Vec3{});
            if (std::isfinite(speedSq) && speedSq <= 225.0f) {
                m_targetVelocity.x = m_targetVelocity.x * 0.35f + velocity.x * 0.65f;
                m_targetVelocity.y = m_targetVelocity.y * 0.35f + velocity.y * 0.65f;
                m_targetVelocity.z = m_targetVelocity.z * 0.35f + velocity.z * 0.65f;
            } else {
                m_targetVelocity = {};
            }
        } else {
            m_targetVelocity = {};
        }
    }
    m_motionTrackedTarget = bestTarget;
    m_lastMotionPosition = finiteVec3(targetPosition) ? targetPosition : Vec3{};
    m_hasMotionSample = finiteVec3(targetPosition);
    m_lastMotionSample = motionSampleTime;
    if (targetChanged) {
        m_hasAimPoint = false;
        m_lastRotationUpdate = {};
    }
    if (targetChanged) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
            "BedrockAim", "target_selection", bestTargetIsPlayer ? "player" : "mob",
            "new target selected; target bounds passed validation");
    }
    m_lastTarget = bestTarget;
    m_lastTargetIsPlayer = bestTargetIsPlayer;
    m_targetPresent = true;
    m_lastTargetBounds = bestTargetBounds;
    m_lastTargetSeen = motionSampleTime;
}


bool AimAssistModule::localPlayerIsGliding() {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    return m_lastPlayer && actorIsGliding(m_lastPlayer);
}

void AimAssistModule::processPlayerFrame() {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    auto* player = m_lastPlayer;
    if (!enabled || !player || !m_lastTarget || !m_targetPresent ||
        !m_weaponFilterAllowsCurrent || !g_applyLocalPlayerTurnDelta) return;

    // Candidate scanning is performed once per player tick. Re-running the native
    // nearby-actor query on every render frame caused duplicate 80-block scans and
    // unnecessary native string/geometry work on low-end Android.
    const auto now = std::chrono::steady_clock::now();
    if (m_lastTargetSeen.time_since_epoch().count() == 0 ||
        now < m_lastTargetSeen ||
        now - m_lastTargetSeen > std::chrono::milliseconds(250) ||
        !validBounds(m_lastTargetBounds)) {
        return;
    }

    applyAimStepLocked(player);
}

void AimAssistModule::applyAimStepLocked(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;

    if (!enabled || !player) return;
    if (!m_lastTarget || !g_applyLocalPlayerTurnDelta) return;
    // Preserve actual gliding state for target selection; Elytra mode and Sticky
    // are separate gates for the special aim-point dislocation below.
    m_localPlayerGliding = actorIsGliding(player);
    const auto now = std::chrono::steady_clock::now();

    auto* rotationComponent = player->rotationComponent();
    if (!rotationComponent || !validBounds(m_lastTargetBounds)) return;

    Vec2 currentRotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(currentRotation.x) || !std::isfinite(currentRotation.y)) return;
    currentRotation.x = std::clamp(currentRotation.x, -89.0f, 89.0f);
    currentRotation.y = wrapDegrees(currentRotation.y);

    float manualYawDelta = 0.0f;
    float manualPitchDelta = 0.0f;
    if (m_hasObservedRotation) {
        manualYawDelta = wrapDegrees(
            wrapDegrees(currentRotation.y - m_lastObservedRotation.y) - m_lastAssistYawDelta);
        manualPitchDelta = (currentRotation.x - m_lastObservedRotation.x) - m_lastAssistPitchDelta;
    }
    m_lastObservedRotation = currentRotation;
    m_hasObservedRotation = true;
    m_lastAssistYawDelta = 0.0f;
    m_lastAssistPitchDelta = 0.0f;

    const Vec3 playerPosition = player->position();
    if (!finiteVec3(playerPosition)) return;
    if (m_hasPredictionPlayerSample) {
        const float sampleDelta = std::chrono::duration<float>(
            now - m_lastPredictionPlayerSample).count();
        if (std::isfinite(sampleDelta) && sampleDelta >= 0.004f && sampleDelta <= 0.25f) {
            Vec3 velocity{
                (playerPosition.x - m_lastPredictionPlayerPosition.x) / sampleDelta,
                (playerPosition.y - m_lastPredictionPlayerPosition.y) / sampleDelta,
                (playerPosition.z - m_lastPredictionPlayerPosition.z) / sampleDelta
            };
            const float speedSquared = distanceSquared(velocity, Vec3{});
            if (std::isfinite(speedSquared) && speedSquared <= 400.0f) {
                m_playerVelocity = {
                    std::clamp(velocity.x, -20.0f, 20.0f),
                    std::clamp(velocity.y, -20.0f, 20.0f),
                    std::clamp(velocity.z, -20.0f, 20.0f)
                };
            } else {
                m_playerVelocity = {};
            }
        } else {
            m_playerVelocity = {};
        }
    } else {
        m_playerVelocity = {};
    }
    m_lastPredictionPlayerPosition = playerPosition;
    m_lastPredictionPlayerSample = now;
    m_hasPredictionPlayerSample = true;

    const AABB playerBounds = player->bounds();
    Vec3 eyePosition = playerPosition;
    if (validBounds(playerBounds)) {
        eyePosition.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        eyePosition.y = playerBounds.min.y + (playerBounds.max.y - playerBounds.min.y) * 0.85f;
        eyePosition.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }

    const AABB nativeBounds = m_lastTargetBounds;
    const HeldWeapon weaponForPrediction = static_cast<HeldWeapon>(std::clamp(
        m_cachedHeldWeaponCode, 0, static_cast<int>(HeldWeapon::Tool)));
    const bool rangedWeapon = bedaim::aimpolicy::isRangedPredictionWeapon(weaponForPrediction);
    const bool usePrediction = m_predictionMode == "All" ||
        (m_predictionMode == "Ranged" && rangedWeapon);

    Vec3 predictionOffset{};
    if (usePrediction && m_hasMotionSample && m_motionTrackedTarget == m_lastTarget) {
        const float leadSeconds = std::clamp(m_predictionLeadMs, 0.0f, 300.0f) / 1000.0f;
        // Aim prediction uses target velocity relative to our own movement. A
        // moving player therefore leads their travel even when the target is still.
        predictionOffset = {
            std::clamp((m_targetVelocity.x - m_playerVelocity.x) * leadSeconds, -3.0f, 3.0f),
            std::clamp((m_targetVelocity.y - m_playerVelocity.y) * leadSeconds, -2.0f, 2.0f),
            std::clamp((m_targetVelocity.z - m_playerVelocity.z) * leadSeconds, -3.0f, 3.0f)
        };
    }

    // Prediction translates the point we aim toward; it does not modify the
    // target's stored/native bounds used by automatic attack range checks.
    AABB targetAimBounds{
        {nativeBounds.min.x + predictionOffset.x, nativeBounds.min.y + predictionOffset.y,
         nativeBounds.min.z + predictionOffset.z},
        {nativeBounds.max.x + predictionOffset.x, nativeBounds.max.y + predictionOffset.y,
         nativeBounds.max.z + predictionOffset.z}
    };
    auto* steeringExpanderBase = ModuleRegistry::get().find("bedrocktools.Hitbox Expander");
    auto* steeringExpander = (steeringExpanderBase && steeringExpanderBase->enabled)
        ? static_cast<HitboxExpanderModule*>(steeringExpanderBase) : nullptr;
    AABB bounds = nativeBounds;
    if (steeringExpander) {
        const bool targetIsPlayer = m_lastTargetIsPlayer;
        bounds = steeringExpander->expandedTargetBounds(targetIsPlayer, nativeBounds);
    }
    bounds.min.x += predictionOffset.x; bounds.max.x += predictionOffset.x;
    bounds.min.y += predictionOffset.y; bounds.max.y += predictionOffset.y;
    bounds.min.z += predictionOffset.z; bounds.max.z += predictionOffset.z;

    const Vec3 boundsCenter{
        (bounds.min.x + bounds.max.x) * 0.5f,
        (bounds.min.y + bounds.max.y) * 0.5f,
        (bounds.min.z + bounds.max.z) * 0.5f
    };
    float aimPointY = 0.55f;
    switch (weaponForPrediction) {
        case HeldWeapon::Sword: aimPointY = m_swordAimPointY; break;
        case HeldWeapon::Axe: aimPointY = m_axeAimPointY; break;
        case HeldWeapon::Mace: aimPointY = m_maceAimPointY; break;
        case HeldWeapon::Bow: aimPointY = m_bowAimPointY; break;
        case HeldWeapon::Crossbow: aimPointY = m_crossbowAimPointY; break;
        case HeldWeapon::Trident: aimPointY = m_tridentAimPointY; break;
        case HeldWeapon::Spear: aimPointY = m_spearAimPointY; break;
        case HeldWeapon::OtherMelee: aimPointY = m_otherMeleeAimPointY; break;
        case HeldWeapon::Tool:
        case HeldWeapon::None: break;
    }
    aimPointY = std::clamp(std::isfinite(aimPointY) ? aimPointY : 0.55f, 0.05f, 0.95f);
    const bedaim::aimmath::Bounds nativeAimMathBounds{
        {targetAimBounds.min.x, targetAimBounds.min.y, targetAimBounds.min.z},
        {targetAimBounds.max.x, targetAimBounds.max.y, targetAimBounds.max.z}
    };
    const auto mathAimPoint = bedaim::aimmath::aimPointAtHeight(nativeAimMathBounds, aimPointY);
    Vec3 centerAimPoint{mathAimPoint.x, mathAimPoint.y, mathAimPoint.z};

    const float yawRadians = currentRotation.y / kRadToDeg;
    const float pitchRadians = currentRotation.x / kRadToDeg;
    const float cosPitch = std::cos(pitchRadians);
    const Vec3 viewDirection{
        -std::sin(yawRadians) * cosPitch,
        -std::sin(pitchRadians),
        std::cos(yawRadians) * cosPitch
    };
    if (m_fourPointAiming) {
        // Pick by distance to the forward view ray (not distance to one projection
        // of the box center). Vertical Y is chosen first and remains independent
        // of horizontal Smooth hitbox sliding.
        const auto anchors = bedaim::aimmath::fourAimAnchors(nativeAimMathBounds, aimPointY);
        const bedaim::aimmath::Vec3 mathEye{eyePosition.x, eyePosition.y, eyePosition.z};
        const bedaim::aimmath::Vec3 mathViewDirection{
            viewDirection.x, viewDirection.y, viewDirection.z
        };
        const auto& selectedAnchor = anchors[bedaim::aimmath::nearestAnchorToRay(
            mathEye, mathViewDirection, anchors)];
        centerAimPoint = {selectedAnchor.x, selectedAnchor.y, selectedAnchor.z};
    }

    // Keep the aim point inside the selected native/virtual hitbox. Elytra
    // lookdown/dislocation was intentionally removed; Unglide lives in Mace Swapper.
    const bool stickyMode = (m_trackingMode == "Grudado");

    const float toCenterX = boundsCenter.x - eyePosition.x;
    const float toCenterY = boundsCenter.y - eyePosition.y;
    const float toCenterZ = boundsCenter.z - eyePosition.z;
    const float alongView = std::max(0.0f,
        toCenterX * viewDirection.x + toCenterY * viewDirection.y + toCenterZ * viewDirection.z);
    const Vec3 projectedPoint{
        eyePosition.x + viewDirection.x * alongView,
        eyePosition.y + viewDirection.y * alongView,
        eyePosition.z + viewDirection.z * alongView
    };
    // Distance-dependent Hitbox Area is a virtual steering volume. At close
    // range it tightens the pull; at long range the strength slider controls how
    // far the volume expands. Native Bedrock collision bounds are never changed.
    const float baseHitboxScale = std::clamp(m_hitboxScale, 0.30f, 50.0f);
    const float hitboxDistance = std::sqrt(std::max(0.0f, distanceSquared(eyePosition, boundsCenter)));
    const float hitboxScale = bedaim::aimmath::distanceHitboxScale(
        baseHitboxScale, hitboxDistance, m_range, m_hitboxDistanceGrowth,
        m_distanceDependentHitbox);
    const Vec3 halfExtents{
        (bounds.max.x - bounds.min.x) * 0.5f * hitboxScale,
        (bounds.max.y - bounds.min.y) * 0.5f * hitboxScale,
        (bounds.max.z - bounds.min.z) * 0.5f * hitboxScale
    };

    const bool adaptiveMode = (m_trackingMode == "Adaptativo");
    const AABB scaledAimBounds{
        {boundsCenter.x - halfExtents.x, boundsCenter.y - halfExtents.y,
         boundsCenter.z - halfExtents.z},
        {boundsCenter.x + halfExtents.x, boundsCenter.y + halfExtents.y,
         boundsCenter.z + halfExtents.z}
    };

    // In Smooth mode, do not keep steering toward the center once the crosshair
    // ray already intersects the configured hitbox area. This lets the crosshair
    // move across the hitbox naturally instead of magnetically snapping to center.
    // Smooth mode keeps the free-slide dead-zone only when the view ray is already
    // close to this weapon's chosen vertical point. Merely crossing the expanded
    // box at its center must not cancel a higher/lower Y profile.
    const float nativeTargetHeight = targetAimBounds.max.y - targetAimBounds.min.y;
    const float verticalAimTolerance = std::max(0.05f, nativeTargetHeight * 0.08f);
    const bool viewIsAtSelectedAimHeight =
        std::abs(projectedPoint.y - centerAimPoint.y) <= verticalAimTolerance;
    if (!stickyMode && viewIsAtSelectedAimHeight &&
        rayIntersectsAABB(eyePosition, viewDirection, scaledAimBounds)) {
        m_hasAimPoint = false;
        m_lastRotationUpdate = now;
        return;
    }

    const Vec3 clampedSlidePoint{
        std::clamp(projectedPoint.x, boundsCenter.x - halfExtents.x, boundsCenter.x + halfExtents.x),
        centerAimPoint.y,
        std::clamp(projectedPoint.z, boundsCenter.z - halfExtents.z, boundsCenter.z + halfExtents.z)
    };
    // Never let the point fully follow the camera ray: that makes aim error collapse
    // to zero inside the hitbox and then jump when the ray exits its edge. Blending
    // from the stable center preserves a steering signal while allowing visible slide.
    constexpr float kHitboxSlideBlend = 0.42f;
    Vec3 slidePoint{
        centerAimPoint.x + (clampedSlidePoint.x - centerAimPoint.x) * kHitboxSlideBlend,
        centerAimPoint.y + (clampedSlidePoint.y - centerAimPoint.y) * kHitboxSlideBlend,
        centerAimPoint.z + (clampedSlidePoint.z - centerAimPoint.z) * kHitboxSlideBlend
    };

    // The scaled AABB controls the soft-lock/sliding region, but the actual aim
    // point must remain inside Bedrock's real bounds or a hit can miss entirely.
    slidePoint.x = std::clamp(slidePoint.x, targetAimBounds.min.x, targetAimBounds.max.x);
    slidePoint.y = std::clamp(slidePoint.y, targetAimBounds.min.y, targetAimBounds.max.y);
    slidePoint.z = std::clamp(slidePoint.z, targetAimBounds.min.z, targetAimBounds.max.z);

    float deltaSeconds = 1.0f / 60.0f;
    if (m_lastRotationUpdate.time_since_epoch().count() != 0) {
        deltaSeconds = std::clamp(
            std::chrono::duration<float>(now - m_lastRotationUpdate).count(),
            0.002f, 0.05f);
    }
    m_lastRotationUpdate = now;

    if (!m_hasAimPoint) {
        m_lastAimPoint = centerAimPoint;
        m_hasAimPoint = true;
    }
    const float slideAlpha = stickyMode ? 1.0f : (1.0f - std::exp(-12.0f * deltaSeconds));
    // Preserve the established Grudado behavior. The configured virtual hitbox
    // selects the sliding point when distance-dependent hitbox is enabled; disabling
    // it restores center-lock. Smooth/Adaptativo keep their existing interpolation.
    // Sticky follows a stable center/four-point anchor regardless of distance hitbox.
    // Smooth and Adaptive retain horizontal hitbox sliding.
    const Vec3 effectiveAimPoint = stickyMode ? centerAimPoint : slidePoint;
    m_lastAimPoint.x += (effectiveAimPoint.x - m_lastAimPoint.x) * slideAlpha;
    m_lastAimPoint.y += (effectiveAimPoint.y - m_lastAimPoint.y) * slideAlpha;
    m_lastAimPoint.z += (effectiveAimPoint.z - m_lastAimPoint.z) * slideAlpha;
    m_lastAimPoint.x = std::clamp(m_lastAimPoint.x, targetAimBounds.min.x, targetAimBounds.max.x);
    m_lastAimPoint.y = std::clamp(m_lastAimPoint.y, targetAimBounds.min.y, targetAimBounds.max.y);
    m_lastAimPoint.z = std::clamp(m_lastAimPoint.z, targetAimBounds.min.z, targetAimBounds.max.z);

    const float aimDx = m_lastAimPoint.x - eyePosition.x;
    const float aimDy = m_lastAimPoint.y - eyePosition.y;
    const float aimDz = m_lastAimPoint.z - eyePosition.z;
    const float aimHorizontal = std::sqrt(aimDx * aimDx + aimDz * aimDz);
    const float desiredYaw = std::atan2(-aimDx, aimDz) * kRadToDeg;
    const float desiredPitch = -std::atan2(aimDy, std::max(aimHorizontal, 0.001f)) * kRadToDeg;
    const float yawError = wrapDegrees(desiredYaw - currentRotation.y);
    const float pitchError = desiredPitch - currentRotation.x;
    if (!std::isfinite(yawError) || !std::isfinite(pitchError)) return;

    float weaponAimMultiplier = 1.0f;
    switch (weaponForPrediction) {
        case HeldWeapon::Sword: weaponAimMultiplier = m_swordAimMultiplier; break;
        case HeldWeapon::Axe: weaponAimMultiplier = m_axeAimMultiplier; break;
        case HeldWeapon::Mace: weaponAimMultiplier = m_maceAimMultiplier; break;
        case HeldWeapon::Bow: weaponAimMultiplier = m_bowAimMultiplier; break;
        case HeldWeapon::Crossbow: weaponAimMultiplier = m_crossbowAimMultiplier; break;
        case HeldWeapon::Trident: weaponAimMultiplier = m_tridentAimMultiplier; break;
        case HeldWeapon::Spear: weaponAimMultiplier = m_spearAimMultiplier; break;
        case HeldWeapon::OtherMelee: weaponAimMultiplier = m_otherMeleeAimMultiplier; break;
        case HeldWeapon::Tool: weaponAimMultiplier = 1.0f; break;
        case HeldWeapon::None: break;
    }
    weaponAimMultiplier = std::clamp(weaponAimMultiplier, 0.2f, 10.0f);
    const float aimSpeed = std::clamp(m_aimSpeed * weaponAimMultiplier, 0.1f, 100.0f);
    const float rotationSpeed = std::clamp(m_rotationSpeed, 0.1f, 1000.0f);
    float yawDelta = 0.0f;
    float pitchDelta = 0.0f;
    const float baseTurnRate = (70.0f + 58.0f * aimSpeed) * rotationSpeed;

    if (stickyMode) {
        // Aim profile applies in every tracking mode, including Sticky.
        float profileRate = 1.0f;
        if (m_aimMode == "Robotic") profileRate = 1.15f;
        else if (m_aimMode == "Human") profileRate = 0.78f;
        const float stickyTurnRate = baseTurnRate * profileRate;
        yawDelta = bedaim::aimmath::boundedTurnStep(yawError, stickyTurnRate, deltaSeconds);
        pitchDelta = bedaim::aimmath::boundedTurnStep(pitchError, stickyTurnRate, deltaSeconds);
    } else {
        // Smooth mode uses time-based easing with no hard-coded 30-FPS step.
        float modeResponse = 1.0f;
        float modeRate = 1.0f;
        if (m_aimMode == "Robotic") {
            modeResponse = 1.35f;
            modeRate = 1.15f;
        } else if (m_aimMode == "Human") {
            modeResponse = 0.68f;
            modeRate = 0.78f;
        }
        // Adaptive is error-aware smoothing, not target prediction: it responds
        // more decisively to large angular errors and eases down near alignment.
        const float errorMagnitude = std::sqrt(yawError * yawError + pitchError * pitchError);
        const float normalizedError = std::clamp(errorMagnitude / 12.0f, 0.0f, 1.0f);
        const float adaptiveResponse = adaptiveMode ? (0.45f + 0.55f * normalizedError) : 1.0f;
        const float adaptiveRate = adaptiveMode ? (0.65f + 0.35f * normalizedError) : 1.0f;
        const float response = (3.0f + 2.6f * aimSpeed) * modeResponse * adaptiveResponse;
        const float maxTurnRate = baseTurnRate * modeRate * adaptiveRate;
        const float alpha = 1.0f - std::exp(-response * deltaSeconds);
        yawDelta = bedaim::aimmath::boundedTurnStep(yawError * alpha, maxTurnRate, deltaSeconds);
        pitchDelta = bedaim::aimmath::boundedTurnStep(pitchError * alpha, maxTurnRate, deltaSeconds);
    }

    // Strongness is not aim speed: it controls how much the assist resists an
    // opposing manual drag. At 0, meaningful opposing input can fully suppress
    // the assist for that update; at 1, normal tracking strength is maintained.
    const float manualMagnitude = std::sqrt(
        manualYawDelta * manualYawDelta + manualPitchDelta * manualPitchDelta);
    const float errorMagnitude = std::sqrt(yawError * yawError + pitchError * pitchError);
    if (manualMagnitude > 0.12f && errorMagnitude > 0.01f &&
        std::isfinite(manualMagnitude) && std::isfinite(errorMagnitude)) {
        const float alignment = (manualYawDelta * yawError + manualPitchDelta * pitchError)
            / (manualMagnitude * errorMagnitude);
        const float opposingInput = std::clamp(-alignment, 0.0f, 1.0f);
        const float inputConfidence = std::clamp((manualMagnitude - 0.12f) / 1.0f, 0.0f, 1.0f);
        const float strongness = std::clamp(m_strength, 0.0f, 1.0f);
        const float dragScale = 1.0f - opposingInput * inputConfidence * (1.0f - strongness);
        yawDelta *= dragScale;
        pitchDelta *= dragScale;
    }

    // Naturalization normally suppresses vertical correction briefly after a jump,
    // but must not block tracking when the selected target is below the player's eyes.
    // This is common during mace Wind Burst/ground-slam arcs.
    const HeldWeapon heldWeapon = weaponForPrediction;
    const bool naturalizationForWeapon =
        m_naturalization && naturalizationAppliesToWeapon(m_naturalizationWeapon, heldWeapon);
    const bool targetBelowView = boundsCenter.y < eyePosition.y - 0.35f;
    if (naturalizationForWeapon && now < m_pitchHoldUntil && !targetBelowView)
        pitchDelta = 0.0f;

    if (!std::isfinite(yawDelta) || !std::isfinite(pitchDelta)) return;
    if (yawDelta == 0.0f && pitchDelta == 0.0f) return;

    Vec2 turnDelta{-pitchDelta, yawDelta};
    g_applyLocalPlayerTurnDelta(player, &turnDelta);
    m_lastAssistYawDelta = turnDelta.y;
    m_lastAssistPitchDelta = turnDelta.x;
}

void AimAssistModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::unique_lock<std::mutex> stateLock(m_stateMutex);
    m_targetPlayers = j.value("m_targetPlayers", m_targetPlayers);
    m_targetMobs = j.value("m_targetMobs", m_targetMobs);
    m_ignoreCreativeSpectator = j.value("m_ignoreCreativeSpectator", m_ignoreCreativeSpectator);
    m_cantSeeThroughBlocks = j.value("m_cantSeeThroughBlocks", m_cantSeeThroughBlocks);
    m_aimMode = j.value("m_aimMode", m_aimMode);
    m_trackingMode = j.value("m_trackingMode", m_trackingMode);
    m_naturalization = j.value("m_naturalization", m_naturalization);
    m_naturalizationWeapon = j.value("m_naturalizationWeapon", m_naturalizationWeapon);
    m_targetSelectionMode = j.value("m_targetSelectionMode", m_targetSelectionMode);

    // LeviLauncher Radio callbacks send the selected option index ("0", "1", ...),
    // not necessarily the displayed label. Accept both the index and legacy label
    // so changes take effect immediately and older saved configs remain compatible.
    if (m_aimMode == "0") m_aimMode = "Robotic";
    else if (m_aimMode == "1") m_aimMode = "Classic";
    else if (m_aimMode == "2") m_aimMode = "Human";
    if (m_aimMode != "Robotic" && m_aimMode != "Classic" && m_aimMode != "Human") m_aimMode = "Classic";

    if (m_trackingMode == "0") m_trackingMode = "Grudado";
    else if (m_trackingMode == "1") m_trackingMode = "Smooth";
    else if (m_trackingMode == "2") m_trackingMode = "Adaptativo";
    // Some menu builds serialize the visible option label instead of its value.
    if (m_trackingMode == "Sticky" || m_trackingMode == "sticky") m_trackingMode = "Grudado";
    else if (m_trackingMode == "Adaptive" || m_trackingMode == "adaptive") m_trackingMode = "Adaptativo";
    if (m_trackingMode != "Grudado" && m_trackingMode != "Smooth" &&
        m_trackingMode != "Adaptativo") m_trackingMode = "Smooth";

    if (m_targetSelectionMode == "0") m_targetSelectionMode = "Player";
    else if (m_targetSelectionMode == "1") m_targetSelectionMode = "Crosshair";
    if (m_targetSelectionMode != "Player" && m_targetSelectionMode != "Crosshair")
        m_targetSelectionMode = "Crosshair";

    if (m_naturalizationWeapon == "0") m_naturalizationWeapon = "All";
    else if (m_naturalizationWeapon == "1") m_naturalizationWeapon = "Sword";
    else if (m_naturalizationWeapon == "2") m_naturalizationWeapon = "Axe";
    else if (m_naturalizationWeapon == "3") m_naturalizationWeapon = "Mace";
    else if (m_naturalizationWeapon == "4") m_naturalizationWeapon = "Bow";
    else if (m_naturalizationWeapon == "5") m_naturalizationWeapon = "Crossbow";
    else if (m_naturalizationWeapon == "6") m_naturalizationWeapon = "Trident";
    else if (m_naturalizationWeapon == "7") m_naturalizationWeapon = "Spear";
    else if (m_naturalizationWeapon == "8") m_naturalizationWeapon = "Other melee";
    else if (m_naturalizationWeapon == "9") m_naturalizationWeapon = "Tools";
    if (m_naturalizationWeapon != "All" && m_naturalizationWeapon != "Sword" &&
        m_naturalizationWeapon != "Axe" && m_naturalizationWeapon != "Mace" &&
        m_naturalizationWeapon != "Bow" && m_naturalizationWeapon != "Crossbow" &&
        m_naturalizationWeapon != "Trident" && m_naturalizationWeapon != "Spear" &&
        m_naturalizationWeapon != "Other melee" && m_naturalizationWeapon != "Tools") m_naturalizationWeapon = "All";
    m_weaponOnly = j.value("m_weaponOnly", m_weaponOnly);
    m_weaponSword = j.value("m_weaponSword", m_weaponSword);
    m_weaponAxe = j.value("m_weaponAxe", m_weaponAxe);
    m_weaponMace = j.value("m_weaponMace", m_weaponMace);
    m_weaponBow = j.value("m_weaponBow", m_weaponBow);
    m_weaponCrossbow = j.value("m_weaponCrossbow", m_weaponCrossbow);
    m_weaponTrident = j.value("m_weaponTrident", m_weaponTrident);
    m_weaponSpear = j.value("m_weaponSpear", m_weaponSpear);
    m_weaponOtherMelee = j.value("m_weaponOtherMelee", m_weaponOtherMelee);
    m_range = std::clamp(j.value("m_range", m_range), 2.0f, 80.0f);
    m_fov = std::clamp(j.value("m_fov", m_fov), 1.0f, 360.0f);
    m_targetSwitchHysteresis = std::clamp(j.value("m_targetSwitchHysteresis", m_targetSwitchHysteresis), 0.0f, 12.0f);
    m_targetSwitchDistanceMargin = std::clamp(j.value("m_targetSwitchDistanceMargin", m_targetSwitchDistanceMargin), 0.0f, 5.0f);
    m_targetRetentionRadius = std::clamp(j.value("m_targetRetentionRadius", m_targetRetentionRadius), 0.5f, 5.0f);
    m_hitboxScale = std::clamp(j.value("m_hitboxScale", m_hitboxScale), 0.30f, 50.0f);
    m_distanceDependentHitbox = j.value("m_distanceDependentHitbox", m_distanceDependentHitbox);
    // A marker distinguishes old percentage-based configs from the expanded
    // scale, so values above 10 remain usable after every menu change.
    const bool usesExpandedGrowthScale = j.value("m_hitboxGrowthScaleV2", false);
    const float savedHitboxGrowth = j.value("m_hitboxDistanceGrowth", m_hitboxDistanceGrowth);
    float migratedHitboxGrowth = savedHitboxGrowth;
    if (!usesExpandedGrowthScale) {
        migratedHitboxGrowth = savedHitboxGrowth > 10.0f
            ? savedHitboxGrowth / 100.0f
            : savedHitboxGrowth;
        // Migrate the exact legacy default once. Values above 10 can now be
        // legitimate user choices in the expanded range.
        if (savedHitboxGrowth >= 99.5f && savedHitboxGrowth <= 100.5f)
            migratedHitboxGrowth = 5.0f;
    }
    m_hitboxDistanceGrowth = std::clamp(migratedHitboxGrowth, 0.1f, 500.0f);
    m_hitboxGrowthScaleV2 = true;
    m_rotationSpeed = std::clamp(j.value("m_rotationSpeed", m_rotationSpeed), 0.1f, 1000.0f);
    m_predictionMode = j.value("m_predictionMode", m_predictionMode);
    if (m_predictionMode == "0") m_predictionMode = "Off";
    else if (m_predictionMode == "1") m_predictionMode = "Ranged";
    else if (m_predictionMode == "2") m_predictionMode = "All";
    if (m_predictionMode == "Ranged weapons" || m_predictionMode == "ranged weapons") m_predictionMode = "Ranged";
    else if (m_predictionMode == "All weapons" || m_predictionMode == "all weapons") m_predictionMode = "All";
    if (m_predictionMode != "Off" && m_predictionMode != "Ranged" && m_predictionMode != "All")
        m_predictionMode = "Ranged";
    m_predictionLeadMs = std::clamp(j.value("m_predictionLeadMs", m_predictionLeadMs), 0.0f, 300.0f);
    m_fourPointAiming = j.value("m_fourPointAiming", m_fourPointAiming);
    m_strength = std::clamp(j.value("m_strength", m_strength), 0.0f, 1.0f);
    m_aimSpeed = std::clamp(j.value("m_aimSpeed", m_aimSpeed), 0.1f, 10.0f);
    m_swordAimMultiplier = std::clamp(j.value("m_swordAimMultiplier", m_swordAimMultiplier), 0.2f, 10.0f);
    m_axeAimMultiplier = std::clamp(j.value("m_axeAimMultiplier", m_axeAimMultiplier), 0.2f, 10.0f);
    m_maceAimMultiplier = std::clamp(j.value("m_maceAimMultiplier", m_maceAimMultiplier), 0.2f, 10.0f);
    m_swordAimPointY = std::clamp(j.value("m_swordAimPointY", m_swordAimPointY), 0.05f, 0.95f);
    m_axeAimPointY = std::clamp(j.value("m_axeAimPointY", m_axeAimPointY), 0.05f, 0.95f);
    m_maceAimPointY = std::clamp(j.value("m_maceAimPointY", m_maceAimPointY), 0.05f, 0.95f);
    m_bowAimPointY = std::clamp(j.value("m_bowAimPointY", m_bowAimPointY), 0.05f, 0.95f);
    m_crossbowAimPointY = std::clamp(j.value("m_crossbowAimPointY", m_crossbowAimPointY), 0.05f, 0.95f);
    m_tridentAimPointY = std::clamp(j.value("m_tridentAimPointY", m_tridentAimPointY), 0.05f, 0.95f);
    m_spearAimPointY = std::clamp(j.value("m_spearAimPointY", m_spearAimPointY), 0.05f, 0.95f);
    m_otherMeleeAimPointY = std::clamp(j.value("m_otherMeleeAimPointY", m_otherMeleeAimPointY), 0.05f, 0.95f);
    m_bowAimMultiplier = std::clamp(j.value("m_bowAimMultiplier", m_bowAimMultiplier), 0.2f, 10.0f);
    m_crossbowAimMultiplier = std::clamp(j.value("m_crossbowAimMultiplier", m_crossbowAimMultiplier), 0.2f, 10.0f);
    m_tridentAimMultiplier = std::clamp(j.value("m_tridentAimMultiplier", m_tridentAimMultiplier), 0.2f, 10.0f);
    m_spearAimMultiplier = std::clamp(j.value("m_spearAimMultiplier", m_spearAimMultiplier), 0.2f, 10.0f);
    m_otherMeleeAimMultiplier = std::clamp(j.value("m_otherMeleeAimMultiplier", m_otherMeleeAimMultiplier), 0.2f, 10.0f);
    m_whitelistNames = j.value("m_whitelistNames", m_whitelistNames);
}

void AimAssistModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    j["m_targetPlayers"] = m_targetPlayers;
    j["m_targetMobs"] = m_targetMobs;
    j["m_ignoreCreativeSpectator"] = m_ignoreCreativeSpectator;
    j["m_cantSeeThroughBlocks"] = m_cantSeeThroughBlocks;
    j["m_aimMode"] = m_aimMode;
    j["m_trackingMode"] = m_trackingMode;
    j["m_naturalization"] = m_naturalization;
    j["m_naturalizationWeapon"] = m_naturalizationWeapon;
    j["m_weaponOnly"] = m_weaponOnly;
    j["m_weaponSword"] = m_weaponSword;
    j["m_weaponAxe"] = m_weaponAxe;
    j["m_weaponMace"] = m_weaponMace;
    j["m_weaponBow"] = m_weaponBow;
    j["m_weaponCrossbow"] = m_weaponCrossbow;
    j["m_weaponTrident"] = m_weaponTrident;
    j["m_weaponSpear"] = m_weaponSpear;
    j["m_weaponOtherMelee"] = m_weaponOtherMelee;
    j["m_range"] = m_range;
    j["m_fov"] = m_fov;
    j["m_targetSwitchHysteresis"] = m_targetSwitchHysteresis;
    j["m_targetSwitchDistanceMargin"] = m_targetSwitchDistanceMargin;
    j["m_targetRetentionRadius"] = m_targetRetentionRadius;
    j["m_targetSelectionMode"] = m_targetSelectionMode;
    j["m_hitboxScale"] = m_hitboxScale;
    j["m_distanceDependentHitbox"] = m_distanceDependentHitbox;
    j["m_hitboxDistanceGrowth"] = m_hitboxDistanceGrowth;
    j["m_hitboxGrowthScaleV2"] = true;
    j["m_strength"] = m_strength;
    j["m_aimSpeed"] = m_aimSpeed;
    j["m_swordAimMultiplier"] = m_swordAimMultiplier;
    j["m_axeAimMultiplier"] = m_axeAimMultiplier;
    j["m_maceAimMultiplier"] = m_maceAimMultiplier;
    j["m_swordAimPointY"] = m_swordAimPointY;
    j["m_axeAimPointY"] = m_axeAimPointY;
    j["m_maceAimPointY"] = m_maceAimPointY;
    j["m_bowAimPointY"] = m_bowAimPointY;
    j["m_crossbowAimPointY"] = m_crossbowAimPointY;
    j["m_tridentAimPointY"] = m_tridentAimPointY;
    j["m_spearAimPointY"] = m_spearAimPointY;
    j["m_otherMeleeAimPointY"] = m_otherMeleeAimPointY;
    j["m_fourPointAiming"] = m_fourPointAiming;
    j["m_bowAimMultiplier"] = m_bowAimMultiplier;
    j["m_crossbowAimMultiplier"] = m_crossbowAimMultiplier;
    j["m_tridentAimMultiplier"] = m_tridentAimMultiplier;
    j["m_spearAimMultiplier"] = m_spearAimMultiplier;
    j["m_otherMeleeAimMultiplier"] = m_otherMeleeAimMultiplier;
    j["m_rotationSpeed"] = m_rotationSpeed;
    j["m_predictionMode"] = m_predictionMode;
    j["m_predictionLeadMs"] = m_predictionLeadMs;
    j["m_whitelistNames"] = m_whitelistNames;
}

 
HitboxExpanderModule::HitboxExpanderModule()
    : Module("BedrockBounds",
             "Temporarily expands client-side actor bounds for native target queries; the server still validates reach and hits.",
             "Hitbox Expander") {
    keybind = 0;
    g_hitboxExpander = this;
}

void HitboxExpanderModule::onInit() {
    if (!g_fetchNearbyActors) {
        g_fetchNearbyActors = reinterpret_cast<ActorFetchNearbyActorsSortedFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));
    }
    if (!g_actorIsPlayer) {
        g_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    }
    const bool queryReady = g_fetchNearbyActors != nullptr;
    const bool actorTypeReady = g_actorIsPlayer != nullptr;
    bedrocktools::diagnostics::probe(
        queryReady && actorTypeReady ? bedrocktools::diagnostics::Level::Info :
            bedrocktools::diagnostics::Level::Warning,
        "HitboxExpander", "native_dependencies",
        queryReady && actorTypeReady ? "ready" : "partial",
        std::string("nearby_query=") + (queryReady ? "ok" : "missing") +
        ", actor_classification=" + (actorTypeReady ? "ok" : "missing"));

    std::call_once(g_hitboxExpanderEventOnce, [] {
        bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerPreTickEvent>(
            [](auto& event) {
                if (g_hitboxExpander) g_hitboxExpander->processPreTick(event.player);
            });
        bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
            [](auto& event) {
                if (g_hitboxExpander) g_hitboxExpander->processPostTick(event.player);
            });
    });
}

void HitboxExpanderModule::onDisable() {
    // Avoid dereferencing a cached actor from a menu callback. The persistent pre-tick
    // listener restores any active snapshots on the next valid in-world tick.
}

void HitboxExpanderModule::processPreTick(bedrocktools::sdk::Player* player) {
    // Keep in-range expanded AABBs stable. Restore only actors that left the
    // active query radius while enabled; restore everything when the module is off.
    if (!player) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "HitboxExpander", "player", "missing");
        return;
    }
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
        "HitboxExpander", "restore_policy", enabled ? "restore_stale_only" : "restore_all");
    restoreNativeBounds(player, enabled);
}

void HitboxExpanderModule::processPostTick(bedrocktools::sdk::Player* player) {
    if (!enabled) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "HitboxExpander", "state", "disabled");
        return;
    }
    if (!player) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "HitboxExpander", "player", "missing");
        return;
    }
    applyNativeBounds(player);
}

void HitboxExpanderModule::restoreNativeBounds(bedrocktools::sdk::Player* player, bool onlyStale) {
    using namespace bedrocktools::sdk;
    if (!player || !g_fetchNearbyActors) return;

    std::vector<void*> trackedActors;
    {
        std::lock_guard<std::mutex> lock(m_nativeBoundsMutex);
        if (m_nativeBoundsOriginal.empty()) return;
        trackedActors.reserve(m_nativeBoundsOriginal.size());
        for (const auto& entry : m_nativeBoundsOriginal) trackedActors.push_back(entry.first);
    }

    // "Active" is the normal 32-block extension query. When enabled, only boxes
    // outside this set are restored so actors no longer handled by the expander do
    // not retain stale expanded bounds. The wider live set below is for pointer safety.
    std::unordered_set<void*> activeActors;
    if (onlyStale && !collectNearbyActors(player, 32.0f, activeActors)) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "HitboxExpander", "restore_query", "active_actor_query_failed");
        return;
    }

    std::unordered_set<void*> liveActors;
    if (!collectNearbyActors(player, 64.0f, liveActors)) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "HitboxExpander", "restore_query", "live_actor_query_failed");
        return;
    }
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
        "HitboxExpander", "restore_query", "ready");
    const auto hasMissingTrackedActor = [&] {
        for (void* actor : trackedActors) {
            if (liveActors.find(actor) == liveActors.end()) return true;
        }
        return false;
    };

    bool missingActorsResolved = !hasMissingTrackedActor();
    if (!missingActorsResolved) {
        if (!collectNearbyActors(player, 256.0f, liveActors)) return;
        missingActorsResolved = !hasMissingTrackedActor();
        if (!missingActorsResolved) {
            if (!collectNearbyActors(player, 512.0f, liveActors)) return;
            missingActorsResolved = true;
        }
    }

    std::lock_guard<std::mutex> lock(m_nativeBoundsMutex);
    for (auto it = m_nativeBoundsOriginal.begin(); it != m_nativeBoundsOriginal.end();) {
        void* actor = it->first;
        const auto snapshot = it->second;
        if (liveActors.find(actor) == liveActors.end()) {
            // A successful broad query is required before dropping an unseen actor.
            // Never dereference a pointer that the current level no longer reports.
            if (missingActorsResolved) it = m_nativeBoundsOriginal.erase(it);
            else ++it;
            continue;
        }

        if (onlyStale && activeActors.find(actor) != activeActors.end()) {
            // This actor is still inside the expander's active radius; keep its
            // enlarged AABB untouched to avoid per-tick native/expanded oscillation.
            ++it;
            continue;
        }

        const auto actorAddress = reinterpret_cast<std::uintptr_t>(actor);
        if (actorAddress <= 0x1000) {
            it = m_nativeBoundsOriginal.erase(it);
            continue;
        }

        auto** componentSlot = reinterpret_cast<void**>(actorAddress + offsets::Actor::mStateVectorComponent +
            offsets::BuiltInActorComponents::mAABBShapeComponent);
        void* currentComponent = *componentSlot;
        auto* actorObject = reinterpret_cast<Actor*>(actor);
        void* currentLevel = actorObject->level();
        const Vec3 currentPosition = actorObject->position();
        if (currentComponent != snapshot.component || currentLevel != snapshot.level ||
            reinterpret_cast<std::uintptr_t>(currentComponent) <= 0x1000 ||
            !finiteVec3(currentPosition) || !finiteVec3(snapshot.originalActorPosition)) {
            it = m_nativeBoundsOriginal.erase(it);
            continue;
        }

        const Vec3 delta{
            currentPosition.x - snapshot.originalActorPosition.x,
            currentPosition.y - snapshot.originalActorPosition.y,
            currentPosition.z - snapshot.originalActorPosition.z
        };
        const AABB translated{
            {snapshot.originalBounds.min.x + delta.x,
             snapshot.originalBounds.min.y + delta.y,
             snapshot.originalBounds.min.z + delta.z},
            {snapshot.originalBounds.max.x + delta.x,
             snapshot.originalBounds.max.y + delta.y,
             snapshot.originalBounds.max.z + delta.z}
        };
        auto* currentBounds = reinterpret_cast<AABB*>(reinterpret_cast<std::uintptr_t>(currentComponent) +
            offsets::AABBShapeComponent::mAABB);
        if (validBounds(translated)) std::memcpy(currentBounds, &translated, sizeof(AABB));

        // Keep the first unexpanded snapshot while enabled. Re-capturing the box
        // after each movement tick would bake any stale world position into the
        // next snapshot and make the hitbox lag behind the actor.
        if (enabled && !onlyStale) ++it;
        else it = m_nativeBoundsOriginal.erase(it);
    }
}

void HitboxExpanderModule::applyNativeBounds(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    if (!enabled || !player || !g_fetchNearbyActors || !g_actorIsPlayer) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "HitboxExpander", "apply_gate", !enabled ? "disabled" :
                !player ? "missing_player" : "native_dependency_missing");
        return;
    }

    Vec3 extent{32.0f, 32.0f, 32.0f};
    ActorVector actors = g_fetchNearbyActors(player, &extent, 1);
    const auto begin = reinterpret_cast<std::uintptr_t>(actors.begin);
    const auto end = reinterpret_cast<std::uintptr_t>(actors.end);
    if (!begin || !end || end < begin ||
        (end - begin) % sizeof(DistanceSortedActor) != 0 ||
        (end - begin) / sizeof(DistanceSortedActor) > 8192) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "HitboxExpander", "actor_snapshot", "malformed");
        return;
    }
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
        "HitboxExpander", "actor_snapshot", "valid");

    bool expandPlayers = false;
    bool expandMobs = false;
    {
        std::lock_guard<std::mutex> lock(m_expandMutex);
        expandPlayers = m_expandPlayers;
        expandMobs = m_expandMobs;
    }

    bool expansionTypesChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_nativeBoundsMutex);
        expansionTypesChanged = m_hasAppliedExpansionTypes &&
            (m_lastAppliedExpandPlayers != expandPlayers || m_lastAppliedExpandMobs != expandMobs);
        m_lastAppliedExpandPlayers = expandPlayers;
        m_lastAppliedExpandMobs = expandMobs;
        m_hasAppliedExpansionTypes = true;
    }

    // If the selected actor types change, restore their original translated boxes
    // once before reapplying the new policy. If expansion is fully disabled by
    // configuration, restore all tracked boxes but retain snapshots for re-enable.
    const std::string policy = std::string(expandPlayers ? "players+" : "players-") +
        (expandMobs ? "_mobs+" : "_mobs-");
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "HitboxExpander", "expansion_policy", policy);
    if (expansionTypesChanged) restoreNativeBounds(player);
    if (!expandPlayers && !expandMobs) return;

    void* playerLevel = reinterpret_cast<Actor*>(player)->level();
    if (!playerLevel) return;

    for (auto* entry = actors.begin; entry < actors.end; ++entry) {
        void* actor = entry->actor;
        if (!actor || actor == player || reinterpret_cast<std::uintptr_t>(actor) <= 0x1000) continue;

        const bool isPlayer = g_actorIsPlayer(actor);
        if (isPlayer ? !expandPlayers : (!expandMobs || !categoryHasMobBit(actor))) continue;

        const auto actorAddress = reinterpret_cast<std::uintptr_t>(actor);
        auto* actorObject = reinterpret_cast<Actor*>(actor);
        void* actorLevel = actorObject->level();
        if (!actorLevel || actorLevel != playerLevel) continue;
        auto** componentSlot = reinterpret_cast<void**>(actorAddress + offsets::Actor::mStateVectorComponent +
            offsets::BuiltInActorComponents::mAABBShapeComponent);
        void* component = *componentSlot;
        if (reinterpret_cast<std::uintptr_t>(component) <= 0x1000) continue;

        auto* nativeBounds = reinterpret_cast<AABB*>(reinterpret_cast<std::uintptr_t>(component) +
            offsets::AABBShapeComponent::mAABB);
        const Vec3 actorPosition = actorObject->position();
        if (!finiteVec3(actorPosition)) continue;

        AABB baseBounds = *nativeBounds;
        {
            std::lock_guard<std::mutex> lock(m_nativeBoundsMutex);
            auto snapshotIt = m_nativeBoundsOriginal.find(actor);
            if (snapshotIt != m_nativeBoundsOriginal.end() &&
                (snapshotIt->second.component != component || snapshotIt->second.level != actorLevel)) {
                m_nativeBoundsOriginal.erase(snapshotIt);
                snapshotIt = m_nativeBoundsOriginal.end();
            }

            if (snapshotIt == m_nativeBoundsOriginal.end()) {
                if (!validBounds(baseBounds)) continue;
                snapshotIt = m_nativeBoundsOriginal.emplace(
                    actor, NativeBoundsSnapshot{component, actorLevel, baseBounds, actorPosition}).first;
            }

            const auto& snapshot = snapshotIt->second;
            if (!validBounds(snapshot.originalBounds) || !finiteVec3(snapshot.originalActorPosition)) continue;
            const Vec3 delta{
                actorPosition.x - snapshot.originalActorPosition.x,
                actorPosition.y - snapshot.originalActorPosition.y,
                actorPosition.z - snapshot.originalActorPosition.z
            };
            baseBounds = {
                {snapshot.originalBounds.min.x + delta.x,
                 snapshot.originalBounds.min.y + delta.y,
                 snapshot.originalBounds.min.z + delta.z},
                {snapshot.originalBounds.max.x + delta.x,
                 snapshot.originalBounds.max.y + delta.y,
                 snapshot.originalBounds.max.z + delta.z}
            };
        }

        if (!validBounds(baseBounds)) continue;
        const AABB expanded = expandedTargetBounds(isPlayer, baseBounds);
        if (!validBounds(expanded) ||
            (expanded.min.x == baseBounds.min.x && expanded.min.y == baseBounds.min.y && expanded.min.z == baseBounds.min.z &&
             expanded.max.x == baseBounds.max.x && expanded.max.y == baseBounds.max.y && expanded.max.z == baseBounds.max.z)) {
            std::memcpy(nativeBounds, &baseBounds, sizeof(AABB));
            continue;
        }
        std::memcpy(nativeBounds, &expanded, sizeof(AABB));
    }
}

bedrocktools::sdk::AABB HitboxExpanderModule::expandedTargetBounds(
    bool isPlayer, const bedrocktools::sdk::AABB& nativeBounds) const {
    using namespace bedrocktools::sdk;
    if (!enabled || !validBounds(nativeBounds)) return nativeBounds;

    float configuredWidth = 1.5f;
    float configuredHeight = 2.5f;
    bool selectedTypeEnabled = false;
    {
        std::lock_guard<std::mutex> lock(m_expandMutex);
        configuredWidth = std::clamp(m_hitboxWidth, 0.6f, 8.0f);
        configuredHeight = std::clamp(m_hitboxHeight, 0.6f, 8.0f);
        selectedTypeEnabled = isPlayer ? m_expandPlayers : m_expandMobs;
    }
    if (!selectedTypeEnabled) return nativeBounds;

    // Width controls X/Z and height controls Y. Preserve larger native sizes.
    const float nativeWidth = nativeBounds.max.x - nativeBounds.min.x;
    const float nativeHeight = nativeBounds.max.y - nativeBounds.min.y;
    const float nativeDepth = nativeBounds.max.z - nativeBounds.min.z;
    const float width = std::max(nativeWidth, configuredWidth);
    const float height = std::max(nativeHeight, configuredHeight);
    const float depth = std::max(nativeDepth, configuredWidth);
    const Vec3 center{
        (nativeBounds.min.x + nativeBounds.max.x) * 0.5f,
        (nativeBounds.min.y + nativeBounds.max.y) * 0.5f,
        (nativeBounds.min.z + nativeBounds.max.z) * 0.5f
    };
    const Vec3 half{width * 0.5f, height * 0.5f, depth * 0.5f};
    // Height extends upward from the target's feet; the base is not moved below
    // the floor, unlike a symmetric expansion of the vertical axis.
    return {
        {center.x - half.x, nativeBounds.min.y, center.z - half.z},
        {center.x + half.x, nativeBounds.min.y + height, center.z + half.z}
    };
}

float HitboxExpanderModule::queryExpansionMargin() const {
    std::lock_guard<std::mutex> lock(m_expandMutex);
    if (!enabled || (!m_expandPlayers && !m_expandMobs)) return 0.0f;
    return std::clamp(0.5f * std::max(
        std::clamp(m_hitboxWidth, 0.6f, 8.0f),
        std::clamp(m_hitboxHeight, 0.6f, 8.0f)), 0.0f, 4.0f);
}

bool HitboxExpanderModule::requiresLineOfSight() const {
    std::lock_guard<std::mutex> lock(m_expandMutex);
    return enabled && m_requireLineOfSight;
}

bool HitboxExpanderModule::shouldShowHitbox() const {
    std::lock_guard<std::mutex> lock(m_expandMutex);
    return enabled && m_showHitbox;
}

bool HitboxExpanderModule::expandsType(bool isPlayer) const {
    std::lock_guard<std::mutex> lock(m_expandMutex);
    return enabled && (isPlayer ? m_expandPlayers : m_expandMobs);
}

bool HitboxExpanderModule::showInLegacyMenu(std::string_view key) const {
    return key != "m_hitboxSize";
}

void HitboxExpanderModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    float legacyScale = m_hitboxSize;
    if (j.contains("m_hitboxSize") && j["m_hitboxSize"].is_number()) {
        const float value = j["m_hitboxSize"].get<float>();
        if (std::isfinite(value)) legacyScale = std::clamp(value, 1.0f, 200.0f);
    }
    legacyScale = std::clamp(legacyScale, 1.0f, 8.0f);
    float width = m_hitboxWidth;
    float height = m_hitboxHeight;
    if (j.contains("m_hitboxWidth") && j["m_hitboxWidth"].is_number()) {
        const float value = j["m_hitboxWidth"].get<float>();
        if (std::isfinite(value)) width = std::clamp(value, 0.6f, 8.0f);
    } else {
        width = std::clamp(std::max(1.5f, 0.6f * legacyScale), 0.6f, 8.0f);
    }
    if (j.contains("m_hitboxHeight") && j["m_hitboxHeight"].is_number()) {
        const float value = j["m_hitboxHeight"].get<float>();
        if (std::isfinite(value)) height = std::clamp(value, 0.6f, 8.0f);
    } else {
        height = std::clamp(std::max(2.5f, 1.8f * legacyScale), 0.6f, 8.0f);
    }
    std::lock_guard<std::mutex> lock(m_expandMutex);
    if (j.contains("m_hitboxSize") && j["m_hitboxSize"].is_number()) {
        const float value = j["m_hitboxSize"].get<float>();
        if (std::isfinite(value)) m_hitboxSize = std::clamp(value, 1.0f, 200.0f);
    }
    m_hitboxWidth = width;
    m_hitboxHeight = height;
    m_expandPlayers = j.value("m_expandPlayers", m_expandPlayers);
    m_expandMobs = j.value("m_expandMobs", m_expandMobs);
    m_requireLineOfSight = j.value("m_requireLineOfSight", m_requireLineOfSight);
    m_showHitbox = j.value("m_showHitbox", m_showHitbox);
}

void HitboxExpanderModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_expandMutex);
    j["m_hitboxSize"] = m_hitboxSize;
    j["m_hitboxWidth"] = m_hitboxWidth;
    j["m_hitboxHeight"] = m_hitboxHeight;
    j["m_expandPlayers"] = m_expandPlayers;
    j["m_expandMobs"] = m_expandMobs;
    j["m_requireLineOfSight"] = m_requireLineOfSight;
    j["m_showHitbox"] = m_showHitbox;
}
