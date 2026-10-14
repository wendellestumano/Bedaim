#include "aimassist.hpp"
#include "core/InventoryAccess.hpp"
#include "core/GameHooks.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cctype>
#include <string>
#include <limits>


namespace {
using ActorIsPlayerFn = bool (*)(void*);
using ActorIsInvisibleFn = bool (*)(void*);
using ActorGetNameTagFn = std::string (*)(void*);
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

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;

float wrapDegrees(float angle) {
    if (!std::isfinite(angle)) return 0.0f;
    angle = std::fmod(angle, 360.0f);
    if (angle > 180.0f) angle -= 360.0f;
    else if (angle < -180.0f) angle += 360.0f;
    return angle;
}

bool finiteVec3(const bedrocktools::sdk::Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
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
    if (begin == end) return false;

    void* item = begin[0];
    if (!item) return false;

    const auto itemAddress = reinterpret_cast<std::uintptr_t>(item);
    const auto type = *reinterpret_cast<const std::uint8_t*>(itemAddress + offsets::DataItem::mType);
    const auto id = *reinterpret_cast<const std::uint16_t*>(itemAddress + offsets::DataItem::mId);
    if (type != 7 || id != 0) return false;

    std::uint64_t flags = 0;
    std::memcpy(&flags, reinterpret_cast<const void*>(itemAddress + offsets::DataItem::mValue), sizeof(flags));
    constexpr std::uint64_t kGlidingFlag = std::uint64_t{1} << 32;
    return (flags & kGlidingFlag) != 0;
}

bool validBounds(const bedrocktools::sdk::AABB& bounds) {
    if (!finiteVec3(bounds.min) || !finiteVec3(bounds.max)) return false;
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
    if (!actor || normalizeName(whitelist).empty()) return false;
    if (isPlayer) {
        const auto* player = reinterpret_cast<bedrocktools::sdk::Player*>(actor);
        if (whitelistContainsName(whitelist, player->name())) return true;
    }
    if (g_actorGetNameTag) {
        const std::string nameTag = g_actorGetNameTag(actor);
        if (whitelistContainsName(whitelist, nameTag)) return true;
    }
    return false;
}

enum class HeldWeapon {
    None, Sword, Axe, Mace, Bow, Crossbow, Trident, Spear, OtherMelee
};

HeldWeapon heldWeaponInMainHand(const bedrocktools::sdk::Player* player) {
    if (!player) return HeldWeapon::None;
    auto** vtable = *reinterpret_cast<void***>(const_cast<bedrocktools::sdk::Player*>(player));
    constexpr std::size_t getCarriedItemIndex = bedrocktools::sdk::offsets::VTable::PlayerGetCarriedItem;
    if (!vtable || !vtable[getCarriedItemIndex]) return HeldWeapon::None;
    using GetCarriedItemFn = const void* (*)(void*);
    const void* stack = reinterpret_cast<GetCarriedItemFn>(vtable[getCarriedItemIndex])(
        const_cast<bedrocktools::sdk::Player*>(player));
    if (!stack) return HeldWeapon::None;

    const auto& inventory = bedrocktools::core::InventoryAccess::get();
    // Check crossbow before bow because some item-name matchers use substrings.
    if (inventory.countStack(stack, "crossbow") > 0) return HeldWeapon::Crossbow;
    if (inventory.countStack(stack, "trident") > 0) return HeldWeapon::Trident;
    if (inventory.countStack(stack, "bow") > 0) return HeldWeapon::Bow;
    if (inventory.countStack(stack, "_sword") > 0) return HeldWeapon::Sword;
    if (inventory.countStack(stack, "_axe") > 0) return HeldWeapon::Axe;
    if (inventory.countStack(stack, "mace") > 0) return HeldWeapon::Mace;
    if (inventory.countStack(stack, "spear") > 0) return HeldWeapon::Spear;

    constexpr const char* otherWeapons[] = {
        "dagger", "katana", "rapier", "scythe", "hammer", "halberd", "glaive", "lance"
    };
    for (const char* identifier : otherWeapons) {
        if (inventory.countStack(stack, identifier) > 0) return HeldWeapon::OtherMelee;
    }
    return HeldWeapon::None;
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
    return false;
}

}

AimAssistModule::AimAssistModule()
    : Module("Aim Assist", "Smooth target tracking for private PvP training worlds.") {
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
}

void AimAssistModule::onInit() {
    g_fetchNearbyActors = reinterpret_cast<ActorFetchNearbyActorsSortedFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));
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







void AimAssistModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    processPlayerTickLocked(player);
}

bool AimAssistModule::currentTargetSnapshot(void*& target, bedrocktools::sdk::AABB& bounds, float& range) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    target = nullptr;
    bounds = {};
    range = std::clamp(m_range, 2.0f, 80.0f);
    if (!enabled || !m_lastPlayer || !m_lastTarget || !m_targetPresent || !validBounds(m_lastTargetBounds)) return false;
    target = m_lastTarget;
    bounds = m_lastTargetBounds;
    return true;
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

bool AimAssistModule::currentTargetInCrosshair(bedrocktools::sdk::Player* player, void* target) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    if (!enabled || !player || player != m_lastPlayer || !target ||
        target != m_lastTarget || !m_targetPresent || !validBounds(m_lastTargetBounds)) return false;

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
    // Small tolerance for mobile touch/camera quantization. This still requires
    // the view ray to pass through the target's immediate hitbox area.
    constexpr float crosshairTolerance = 0.12f;
    const AABB crosshairBounds{
        {m_lastTargetBounds.min.x - crosshairTolerance,
         m_lastTargetBounds.min.y - crosshairTolerance,
         m_lastTargetBounds.min.z - crosshairTolerance},
        {m_lastTargetBounds.max.x + crosshairTolerance,
         m_lastTargetBounds.max.y + crosshairTolerance,
         m_lastTargetBounds.max.z + crosshairTolerance}
    };
    if (!rayIntersectsAABB(eyePosition, viewDirection, crosshairBounds)) return false;

    if (m_cantSeeThroughBlocks) {
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

void AimAssistModule::deferAutoHitFor(float delayMs) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    const auto requested = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(static_cast<int>(std::clamp(delayMs, 80.0f, 1000.0f)));
    if (m_nextAutoHit < requested) m_nextAutoHit = requested;
}

void AimAssistModule::processPlayerTickLocked(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;

    if (!enabled || !player || !g_fetchNearbyActors || (!m_targetPlayers && !m_targetMobs)) {
        resetTrackingState();
        return;
    }

    m_lastPlayer = player;
    m_localPlayerGliding = m_elytraMode && actorIsGliding(player);

    const HeldWeapon heldWeapon = heldWeaponInMainHand(player);
    if (m_weaponOnly && !weaponAllowed(heldWeapon, m_weaponSword, m_weaponAxe, m_weaponMace,
            m_weaponBow, m_weaponCrossbow, m_weaponTrident, m_weaponSpear, m_weaponOtherMelee)) {
        resetTrackingState();
        return;
    }

    auto* rotationComponent = player->rotationComponent();
    if (!rotationComponent) {
        resetTrackingState();
        return;
    }

    void* blockSource = nullptr;
    if (m_cantSeeThroughBlocks) {
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
    Vec3 extent{range, range, range};
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
    float bestScore = std::numeric_limits<float>::max();
    void* retainedTarget = nullptr;
    AABB retainedTargetBounds{};
    float retainedTargetScore = std::numeric_limits<float>::max();
    bool retainedTargetInsideBubble = false;
    Vec3 playerCenter = playerPosition;
    if (validBounds(playerBounds)) {
        playerCenter.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        playerCenter.y = (playerBounds.min.y + playerBounds.max.y) * 0.5f;
        playerCenter.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    }
    const float retentionRadius = std::clamp(m_targetRetentionRadius, 0.5f, 5.0f);
    const float retentionRadiusSq = retentionRadius * retentionRadius;
    const float maxDistanceSq = range * range;
    // Experimental full-range FOV. Range, entity type, visibility, and valid bounds
    // remain required even when the angular limit is set to 360 degrees.
    const float maxFov = std::clamp(m_fov, 1.0f, 360.0f);

    for (auto* entry = actors.begin; entry < actors.end; ++entry) {
        void* candidate = entry->actor;
        if (!candidate || candidate == player) continue;

        const bool isPlayer = g_actorIsPlayer && g_actorIsPlayer(candidate);
        if (isPlayer) {
            if (!m_targetPlayers) continue;
        } else {
            if (!m_targetMobs || !categoryHasMobBit(candidate)) continue;
        }

        if (isWhitelistedActor(candidate, isPlayer, m_whitelistNames)) continue;
        if (g_actorIsInvisible && g_actorIsInvisible(candidate)) continue;

        auto* target = reinterpret_cast<Actor*>(candidate);
        const Vec3 targetPosition = target->position();
        const AABB targetBounds = target->bounds();
        if (!finiteVec3(targetPosition) || !validBounds(targetBounds)) continue;

        // Keep target ranking based on a stable center point. A separate point below
        // slides within the scaled hitbox without making target selection chase the view.
        const Vec3 centerAimPoint{
            (targetBounds.min.x + targetBounds.max.x) * 0.5f,
            targetBounds.min.y + (targetBounds.max.y - targetBounds.min.y) * 0.55f,
            (targetBounds.min.z + targetBounds.max.z) * 0.5f
        };

        // Fail closed when the visibility signature or block source is unavailable.
        // hasClearLineOfSight() returns false if it cannot safely perform the check.
        // If the LOS signature is unavailable for this Minecraft build, don't
        // silently discard every candidate. Enforce LOS only when the checker is live.
        if (m_cantSeeThroughBlocks && blockSource && g_isSolidBlockingBlock &&
            !hasClearLineOfSight(blockSource, eyePosition, centerAimPoint)) continue;

        const float d2 = distanceSquared(eyePosition, centerAimPoint);
        if (!std::isfinite(d2) || d2 > maxDistanceSq || d2 < 0.01f) continue;

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
            insideRetentionBubble = distanceSquared(playerCenter, nearestOnTarget) <= retentionRadiusSq;
        }

        const float dx = centerAimPoint.x - eyePosition.x;
        const float dy = centerAimPoint.y - eyePosition.y;
        const float dz = centerAimPoint.z - eyePosition.z;
        const float horizontal = std::sqrt(dx * dx + dz * dz);
        const float desiredYaw = std::atan2(-dx, dz) * kRadToDeg;
        const float desiredPitch = -std::atan2(dy, std::max(horizontal, 0.001f)) * kRadToDeg;

        const float targetDistance = std::sqrt(d2);
        if (!std::isfinite(targetDistance) || targetDistance <= 0.001f) continue;

        const float yawError = wrapDegrees(desiredYaw - currentRotation.y);
        const float pitchError = desiredPitch - currentRotation.x;
        const float angleScore = std::sqrt(yawError * yawError + pitchError * pitchError);
        if (!std::isfinite(angleScore)) continue;

        // Keep targets directly below the player's feet eligible during a mace
        // wind-burst/ground-slam arc. The range check above still limits this to
        // nearby actors, while the normal FOV remains enforced for other targets.
        const float playerFeetY = validBounds(playerBounds) ? playerBounds.min.y : playerPosition.y;
        const bool candidateBelowPlayer =
            targetBounds.max.y <= playerFeetY + 0.12f;

        // The last target also gets a small 3D retention bubble, including when it
        // passes below the player or behind the camera. This prevents target loss
        // during the fast vertical movement of a slam.
        if (angleScore > maxFov && !insideRetentionBubble && !candidateBelowPlayer) continue;

        // Player mode ranks eligible targets by world distance. Crosshair mode
        // ranks by angular distance to the view, with world distance only breaking ties.
        const bool playerDistanceMode = (m_targetSelectionMode == "Player");
        const float score = playerDistanceMode
            ? targetDistance
            : angleScore + targetDistance * 0.0001f;
        if (candidate == m_lastTarget) {
            retainedTarget = candidate;
            retainedTargetBounds = targetBounds;
            retainedTargetScore = score;
            retainedTargetInsideBubble = insideRetentionBubble || candidateBelowPlayer;
        }
        if (score < bestScore) {
            bestScore = score;
            bestTarget = candidate;
            bestTargetBounds = targetBounds;
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
    }

    if (!bestTarget) {
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
    if (targetChanged) {
        m_hasAimPoint = false;
        m_lastRotationUpdate = {};
    }
    m_lastTarget = bestTarget;
    m_targetPresent = true;
    m_lastTargetBounds = bestTargetBounds;
    m_lastTargetSeen = std::chrono::steady_clock::now();
    // Also check Auto Hit here so it is independent of the client-frame render hook.
    processAutoHitLocked(player);
}


void AimAssistModule::processPlayerFrame() {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    using namespace bedrocktools::sdk;

    auto* player = m_lastPlayer;
    if (!enabled || !player || !m_lastTarget || !g_fetchNearbyActors ||
        (!g_applyLocalPlayerTurnDelta && !m_autoHit)) return;

    // Refresh target bounds at the client-update cadence, rather than following
    // a hitbox sampled only on LocalPlayerTickEvent. Match the pointer in the
    // current native actor list before dereferencing it, so a despawned target
    // is never accessed through the cached pointer.
    const float range = std::clamp(m_range, 2.0f, 80.0f);
    Vec3 extent{range, range, range};
    ActorVector actors = g_fetchNearbyActors(player, &extent, 1);
    const auto beginAddress = reinterpret_cast<std::uintptr_t>(actors.begin);
    const auto endAddress = reinterpret_cast<std::uintptr_t>(actors.end);
    if (!actors.begin || !actors.end || endAddress < beginAddress ||
        (endAddress - beginAddress) % sizeof(DistanceSortedActor) != 0 ||
        (endAddress - beginAddress) / sizeof(DistanceSortedActor) > 4096) {
        // A malformed frame snapshot is not proof that the tracked entity despawned.
        return;
    }

    bool targetFound = false;
    for (auto* entry = actors.begin; entry < actors.end; ++entry) {
        if (entry->actor != m_lastTarget) continue;
        const AABB currentBounds = reinterpret_cast<Actor*>(entry->actor)->bounds();
        if (!validBounds(currentBounds)) return;
        m_lastTargetBounds = currentBounds;
        m_lastTargetSeen = std::chrono::steady_clock::now();
        targetFound = true;
        break;
    }
    if (!targetFound) {
        // LocalPlayerTickEvent performs the authoritative candidate refresh. A
        // transient miss in this frame-level list should not erase a close lock.
        return;
    }

    applyAimStepLocked(player);
}

void AimAssistModule::processAutoHitLocked(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;

    // Keep combat calls on the player-tick path only. The target must have been
    // present in the latest native actor snapshot; never attack a retained/stale pointer.
    if (!m_autoHit || !enabled || !player || player != m_lastPlayer ||
        !m_lastTarget || !m_targetPresent || !validBounds(m_lastTargetBounds)) return;

    const auto now = std::chrono::steady_clock::now();
    if (m_lastTargetSeen.time_since_epoch().count() == 0 ||
        now - m_lastTargetSeen > std::chrono::milliseconds(150)) return;
    if (m_nextAutoHit.time_since_epoch().count() != 0 && now < m_nextAutoHit) return;

    // Only use a GameMode pointer captured by a real engine callback. Do not guess
    // a version-sensitive offset from Player, which caused crashes in earlier builds.
    void* gameMode = bedrocktools::core::gamehooks::gameMode();
    if (!gameMode) return;

    const Vec3 playerPosition = player->position();
    const AABB playerBounds = player->bounds();
    if (!finiteVec3(playerPosition)) return;

    Vec3 eyePosition = playerPosition;
    if (validBounds(playerBounds)) {
        eyePosition.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        eyePosition.y = playerBounds.min.y + (playerBounds.max.y - playerBounds.min.y) * 0.85f;
        eyePosition.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }

    auto* rotationComponent = player->rotationComponent();
    if (!rotationComponent) return;
    Vec2 rotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y)) return;
    rotation.x = std::clamp(rotation.x, -89.0f, 89.0f);
    rotation.y = wrapDegrees(rotation.y);

    const AABB bounds = m_lastTargetBounds;
    const Vec3 targetCenter{
        (bounds.min.x + bounds.max.x) * 0.5f,
        bounds.min.y + (bounds.max.y - bounds.min.y) * 0.55f,
        (bounds.min.z + bounds.max.z) * 0.5f
    };
    const float centerDistanceSq = distanceSquared(eyePosition, targetCenter);
    if (!std::isfinite(centerDistanceSq) || centerDistanceSq < 0.01f) return;

    // Do not require a perfect ray/AABB intersection: that made AutoHit behave like
    // ordinary manual clicking and was the main gate blocking nearby selected targets.
    // Still bound it to the configured aim cone so a target retained behind the player
    // can never be attacked just because it remained in the tracking bubble.
    const float yaw = rotation.y / kRadToDeg;
    const float pitch = rotation.x / kRadToDeg;
    const float cosPitch = std::cos(pitch);
    const Vec3 viewDirection{
        -std::sin(yaw) * cosPitch,
        -std::sin(pitch),
        std::cos(yaw) * cosPitch
    };
    const Vec3 toTarget{
        targetCenter.x - eyePosition.x,
        targetCenter.y - eyePosition.y,
        targetCenter.z - eyePosition.z
    };
    const float centerDistance = std::sqrt(centerDistanceSq);
    const float facingDot = (viewDirection.x * toTarget.x +
                             viewDirection.y * toTarget.y +
                             viewDirection.z * toTarget.z) / centerDistance;
    const float maxAimAngle = std::clamp(m_fov, 1.0f, 120.0f) / kRadToDeg;
    if (!std::isfinite(facingDot) ||
        facingDot < std::cos(maxAimAngle)) return;

    // Aim at the closest point on the target's live bounds. This is less error-prone
    // than always passing the geometric center, especially for tall mobs and edge hits.
    const Vec3 hitPoint{
        std::clamp(eyePosition.x, bounds.min.x, bounds.max.x),
        std::clamp(eyePosition.y, bounds.min.y, bounds.max.y),
        std::clamp(eyePosition.z, bounds.min.z, bounds.max.z)
    };
    const float hitDistanceSq = distanceSquared(eyePosition, hitPoint);
    const float autoHitRange = std::clamp(m_autoHitRange, 2.0f, 6.0f);
    if (!std::isfinite(hitDistanceSq) || hitDistanceSq < 0.01f ||
        hitDistanceSq > autoHitRange * autoHitRange) return;

    if (m_cantSeeThroughBlocks) {
        auto* dimension = player->dimension();
        void* blockSource = dimension ? static_cast<void*>(dimension->blockSource()) : nullptr;
        if (blockSource && g_isSolidBlockingBlock &&
            !hasClearLineOfSight(blockSource, eyePosition, hitPoint)) return;
    }

    const float delayMs = std::clamp(m_autoHitDelayMs, 80.0f, 1000.0f);
    const bool accepted = bedrocktools::core::gamehooks::attack(
        gameMode, m_lastTarget, true, &hitPoint);
    // Avoid hammering the native entry if the engine rejects an attack, while allowing
    // the next tick to recover quickly after a transient false return.
    const float nextDelayMs = accepted ? delayMs : std::min(delayMs, 120.0f);
    m_nextAutoHit = now + std::chrono::milliseconds(static_cast<int>(nextDelayMs));
}

void AimAssistModule::applyAimStepLocked(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;

    if (!enabled || !player) return;
    if (!m_lastTarget || !g_applyLocalPlayerTurnDelta) return;
    m_localPlayerGliding = m_elytraMode && actorIsGliding(player);
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
    const AABB playerBounds = player->bounds();
    Vec3 eyePosition = playerPosition;
    if (validBounds(playerBounds)) {
        eyePosition.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        eyePosition.y = playerBounds.min.y + (playerBounds.max.y - playerBounds.min.y) * 0.85f;
        eyePosition.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }

    AABB bounds = m_lastTargetBounds;

    const Vec3 boundsCenter{
        (bounds.min.x + bounds.max.x) * 0.5f,
        (bounds.min.y + bounds.max.y) * 0.5f,
        (bounds.min.z + bounds.max.z) * 0.5f
    };
    const Vec3 centerAimPoint{
        boundsCenter.x,
        bounds.min.y + (bounds.max.y - bounds.min.y) * 0.55f,
        boundsCenter.z
    };

    const float yawRadians = currentRotation.y / kRadToDeg;
    const float pitchRadians = currentRotation.x / kRadToDeg;
    const float cosPitch = std::cos(pitchRadians);
    const Vec3 viewDirection{
        -std::sin(yawRadians) * cosPitch,
        -std::sin(pitchRadians),
        std::cos(yawRadians) * cosPitch
    };
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

    // Distance-dependent Hitbox Area: keep the tracking region tighter nearby and
    // expand it smoothly with distance. This only affects the assist's virtual aim
    // region; it does not mutate Minecraft's native collision boxes.
    const float baseHitboxScale = std::clamp(m_hitboxScale, 0.5f, 1.0f);
    const float hitboxDistance = std::sqrt(std::max(0.0f, distanceSquared(eyePosition, boundsCenter)));
    const float configuredRange = std::clamp(m_range, 2.0f, 80.0f);
    const float distanceFraction = std::clamp(hitboxDistance / configuredRange, 0.0f, 1.0f);
    // Growth is a percentage: 100 doubles the virtual aim region at full range,
    // 1000 expands it up to 11x. Keep the multiplier bounded even with corrupt config.
    const float growthPercent = std::clamp(m_hitboxDistanceGrowth, 0.0f, 1000.0f);
    const float distanceFactor = m_distanceDependentHitbox
        ? 1.0f + (growthPercent / 100.0f) * distanceFraction
        : 1.0f;
    const float hitboxScale = std::clamp(baseHitboxScale * distanceFactor, 0.35f, 11.0f);
    const Vec3 halfExtents{
        (bounds.max.x - bounds.min.x) * 0.5f * hitboxScale,
        (bounds.max.y - bounds.min.y) * 0.5f * hitboxScale,
        (bounds.max.z - bounds.min.z) * 0.5f * hitboxScale
    };

    const bool stickyMode = (m_trackingMode == "Grudado");
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
    if (!stickyMode && rayIntersectsAABB(eyePosition, viewDirection, scaledAimBounds)) {
        m_hasAimPoint = false;
        m_lastRotationUpdate = now;
        return;
    }

    const Vec3 clampedSlidePoint{
        std::clamp(projectedPoint.x, boundsCenter.x - halfExtents.x, boundsCenter.x + halfExtents.x),
        std::clamp(projectedPoint.y, boundsCenter.y - halfExtents.y, boundsCenter.y + halfExtents.y),
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

    const float targetDistance = std::sqrt(distanceSquared(eyePosition, centerAimPoint));
    if (m_elytraMode && m_localPlayerGliding && std::isfinite(targetDistance) &&
        targetDistance >= 7.0f && targetDistance <= 8.0f) {
        const float targetHeight = bounds.max.y - bounds.min.y;
        slidePoint.y -= std::clamp(targetHeight * 0.15f, 0.12f, 0.28f);
    }

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
    const Vec3 effectiveAimPoint = stickyMode ? centerAimPoint : slidePoint;
    m_lastAimPoint.x += (effectiveAimPoint.x - m_lastAimPoint.x) * slideAlpha;
    m_lastAimPoint.y += (effectiveAimPoint.y - m_lastAimPoint.y) * slideAlpha;
    m_lastAimPoint.z += (effectiveAimPoint.z - m_lastAimPoint.z) * slideAlpha;

    const float aimDx = m_lastAimPoint.x - eyePosition.x;
    const float aimDy = m_lastAimPoint.y - eyePosition.y;
    const float aimDz = m_lastAimPoint.z - eyePosition.z;
    const float aimHorizontal = std::sqrt(aimDx * aimDx + aimDz * aimDz);
    const float desiredYaw = std::atan2(-aimDx, aimDz) * kRadToDeg;
    const float desiredPitch = -std::atan2(aimDy, std::max(aimHorizontal, 0.001f)) * kRadToDeg;
    const float yawError = wrapDegrees(desiredYaw - currentRotation.y);
    const float pitchError = desiredPitch - currentRotation.x;
    if (!std::isfinite(yawError) || !std::isfinite(pitchError)) return;

    const float aimSpeed = std::clamp(m_aimSpeed, 0.1f, 10.0f);
    float yawDelta = 0.0f;
    float pitchDelta = 0.0f;
    const float baseTurnRate = 70.0f + 58.0f * aimSpeed;

    if (stickyMode) {
        // No easing tail: react directly to the current error, while respecting AimSpeed.
        const float maxStep = baseTurnRate * deltaSeconds;
        yawDelta = std::clamp(yawError, -maxStep, maxStep);
        pitchDelta = std::clamp(pitchError, -maxStep, maxStep);
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
        const float maxStep = maxTurnRate * deltaSeconds;
        yawDelta = std::clamp(yawError * alpha, -maxStep, maxStep);
        pitchDelta = std::clamp(pitchError * alpha, -maxStep, maxStep);
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

    // Naturalization suppresses vertical correction during the jump hold, but
    // keeps horizontal tracking active.
    const HeldWeapon heldWeapon = heldWeaponInMainHand(player);
    const bool naturalizationForWeapon =
        m_naturalization && naturalizationAppliesToWeapon(m_naturalizationWeapon, heldWeapon);
    if (naturalizationForWeapon && now < m_pitchHoldUntil) pitchDelta = 0.0f;

    if (!std::isfinite(yawDelta) || !std::isfinite(pitchDelta)) return;
    if (yawDelta == 0.0f && pitchDelta == 0.0f) return;

    Vec2 turnDelta{-pitchDelta, yawDelta};
    g_applyLocalPlayerTurnDelta(player, &turnDelta);
    m_lastAssistYawDelta = turnDelta.y;
    m_lastAssistPitchDelta = turnDelta.x;
}

void AimAssistModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    m_targetPlayers = j.value("m_targetPlayers", m_targetPlayers);
    m_targetMobs = j.value("m_targetMobs", m_targetMobs);
    m_cantSeeThroughBlocks = j.value("m_cantSeeThroughBlocks", m_cantSeeThroughBlocks);
    m_aimMode = j.value("m_aimMode", m_aimMode);
    m_trackingMode = j.value("m_trackingMode", m_trackingMode);
    m_elytraMode = j.value("m_elytraMode", m_elytraMode);
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
    if (m_naturalizationWeapon != "All" && m_naturalizationWeapon != "Sword" &&
        m_naturalizationWeapon != "Axe" && m_naturalizationWeapon != "Mace" &&
        m_naturalizationWeapon != "Bow" && m_naturalizationWeapon != "Crossbow" &&
        m_naturalizationWeapon != "Trident" && m_naturalizationWeapon != "Spear" &&
        m_naturalizationWeapon != "Other melee") m_naturalizationWeapon = "All";
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
    m_hitboxScale = std::clamp(j.value("m_hitboxScale", m_hitboxScale), 0.5f, 1.0f);
    m_distanceDependentHitbox = j.value("m_distanceDependentHitbox", m_distanceDependentHitbox);
    m_hitboxDistanceGrowth = std::clamp(j.value("m_hitboxDistanceGrowth", m_hitboxDistanceGrowth), 0.0f, 1000.0f);
    m_strength = std::clamp(j.value("m_strength", m_strength), 0.0f, 1.0f);
    m_aimSpeed = std::clamp(j.value("m_aimSpeed", m_aimSpeed), 0.1f, 10.0f);
    m_whitelistNames = j.value("m_whitelistNames", m_whitelistNames);
    m_autoHit = j.value("m_autoHit", m_autoHit);
    m_autoHitRange = std::clamp(j.value("m_autoHitRange", m_autoHitRange), 2.0f, 6.0f);
    m_autoHitDelayMs = std::clamp(j.value("m_autoHitDelayMs", m_autoHitDelayMs), 80.0f, 1000.0f);
}

void AimAssistModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    j["m_targetPlayers"] = m_targetPlayers;
    j["m_targetMobs"] = m_targetMobs;
    j["m_cantSeeThroughBlocks"] = m_cantSeeThroughBlocks;
    j["m_aimMode"] = m_aimMode;
    j["m_trackingMode"] = m_trackingMode;
    j["m_elytraMode"] = m_elytraMode;
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
    j["m_strength"] = m_strength;
    j["m_aimSpeed"] = m_aimSpeed;
    j["m_whitelistNames"] = m_whitelistNames;
    j["m_autoHit"] = m_autoHit;
    j["m_autoHitRange"] = m_autoHitRange;
    j["m_autoHitDelayMs"] = m_autoHitDelayMs;
}
