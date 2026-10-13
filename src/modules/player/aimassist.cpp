#include "aimassist.hpp"
#include "core/InventoryAccess.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Memory.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cctype>
#include <string>
#include <limits>


namespace {
using ActorIsPlayerFn = bool (*)(void*);
using ActorIsInvisibleFn = bool (*)(void*);
using ActorGetNameTagFn = std::string (*)(void*);
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


float smoothDampedAxisStep(float error, float deltaSeconds,
                           float maxTurnRate, float smoothTime, float& velocity) {
    if (!std::isfinite(error) || !std::isfinite(deltaSeconds) ||
        !std::isfinite(maxTurnRate) || !std::isfinite(smoothTime) ||
        deltaSeconds <= 0.0f || maxTurnRate <= 0.0f) {
        velocity = 0.0f;
        return 0.0f;
    }
    if (std::abs(error) < 0.015f) {
        velocity = 0.0f;
        return 0.0f;
    }

    const float dt = std::clamp(deltaSeconds, 0.001f, 0.1f);
    smoothTime = std::clamp(smoothTime, 0.05f, 1.0f);
    if (!std::isfinite(velocity) ||
        (error > 0.0f && velocity < 0.0f) ||
        (error < 0.0f && velocity > 0.0f)) {
        velocity = 0.0f;
    }

    // Critically damped SmoothDamp-style step: time-based, with a hard turn-rate
    // cap. Resetting opposing velocity prevents stale momentum from pushing away.
    const float omega = 2.0f / smoothTime;
    const float x = omega * dt;
    const float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float change = -error;
    const float maxChange = maxTurnRate * smoothTime;
    change = std::clamp(change, -maxChange, maxChange);
    const float target = -change;
    const float temp = (velocity + omega * change) * dt;
    velocity = (velocity - omega * temp) * decay;

    float step = target + (change + temp) * decay;
    const float maxStep = std::min(maxTurnRate * dt, 10.0f);
    const float limitedStep = std::clamp(step, -maxStep, maxStep);
    if (limitedStep != step) velocity = limitedStep / dt;
    step = limitedStep;

    if ((error > 0.0f && step > error) || (error < 0.0f && step < error)) {
        step = error;
        velocity = 0.0f;
    }
    if (!std::isfinite(velocity)) {
        velocity = 0.0f;
        return 0.0f;
    }
    return step;
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
    m_lastTarget = nullptr;
    m_lastTargetBounds = {};
    m_lastAimPoint = {};
    m_hasAimPoint = false;
    m_yawVelocity = 0.0f;
    m_pitchVelocity = 0.0f;
    m_previousTargetPosition = {};
    m_targetVelocity = {};
    m_previousTargetSample = {};
    m_lastTargetUpdate = {};
    m_lastRotationUpdate = {};
}

void AimAssistModule::onInit() {
    g_fetchNearbyActors = reinterpret_cast<ActorFetchNearbyActorsSortedFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));
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
    using namespace bedrocktools::sdk;

    if (!enabled || !player || !g_fetchNearbyActors || (!m_targetPlayers && !m_targetMobs)) {
        resetTrackingState();
        return;
    }

    const HeldWeapon heldWeapon = heldWeaponInMainHand(player);
    if (m_weaponOnly && !weaponAllowed(heldWeapon, m_weaponSword, m_weaponAxe, m_weaponMace,
            m_weaponBow, m_weaponCrossbow, m_weaponTrident, m_weaponSpear, m_weaponOtherMelee)) {
        resetTrackingState();
        return;
    }

    auto* rotationComponent = player->rotationComponent();
    auto* level = player->level();
    if (!rotationComponent || !level) {
        resetTrackingState();
        return;
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
    void* actorManager = field<void*>(level, offsets::Level::mActorManager);
    if (!actorManager) {
        resetTrackingState();
        return;
    }

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
    void* retainedTarget = nullptr;
    AABB retainedTargetBounds{};
    float bestScore = std::numeric_limits<float>::max();
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

        const float d2 = distanceSquared(eyePosition, centerAimPoint);
        if (!std::isfinite(d2) || d2 > maxDistanceSq || d2 < 0.01f) continue;

        const float dx = centerAimPoint.x - eyePosition.x;
        const float dy = centerAimPoint.y - eyePosition.y;
        const float dz = centerAimPoint.z - eyePosition.z;
        const float horizontal = std::sqrt(dx * dx + dz * dz);
        const float desiredYaw = std::atan2(-dx, dz) * kRadToDeg;
        const float desiredPitch = -std::atan2(dy, std::max(horizontal, 0.001f)) * kRadToDeg;

        const float targetDistance = std::sqrt(d2);
        if (!std::isfinite(targetDistance) || targetDistance <= 0.001f) continue;

        // Prioritize the candidate closest to the crosshair by angular error.
        // Unlike nearest-player priority, distance is only a tiny tie breaker.
        const float yawError = wrapDegrees(desiredYaw - currentRotation.y);
        const float pitchError = desiredPitch - currentRotation.x;
        const float angleScore = std::sqrt(yawError * yawError + pitchError * pitchError);
        if (!std::isfinite(angleScore) || angleScore > maxFov) continue;

        // Keep selection independent from the moving in-hitbox point to avoid target orbiting.
        if (candidate == m_lastTarget) {
            retainedTarget = candidate;
            retainedTargetBounds = targetBounds;
        }

        const float score = angleScore + targetDistance * 0.0001f;
        if (score < bestScore) {
            bestScore = score;
            bestTarget = candidate;
            bestTargetBounds = targetBounds;
        }
    }

    if (retainedTarget) {
        bestTarget = retainedTarget;
        bestTargetBounds = retainedTargetBounds;
    }

    if (!bestTarget) {
        resetTrackingState();
        return;
    }

    const auto sampleNow = std::chrono::steady_clock::now();
    const bool targetChanged = bestTarget != m_lastTarget;
    const Vec3 targetCenter{
        (bestTargetBounds.min.x + bestTargetBounds.max.x) * 0.5f,
        (bestTargetBounds.min.y + bestTargetBounds.max.y) * 0.5f,
        (bestTargetBounds.min.z + bestTargetBounds.max.z) * 0.5f
    };

    if (targetChanged) {
        m_yawVelocity = 0.0f;
        m_pitchVelocity = 0.0f;
        m_targetVelocity = {};
        m_previousTargetSample = {};
        m_hasAimPoint = false;
        m_lastRotationUpdate = {};
    } else if (m_previousTargetSample.time_since_epoch().count() != 0) {
        const float sampleDt = std::chrono::duration<float>(sampleNow - m_previousTargetSample).count();
        if (std::isfinite(sampleDt) && sampleDt >= 0.01f && sampleDt <= 0.25f) {
            Vec3 measuredVelocity{
                (targetCenter.x - m_previousTargetPosition.x) / sampleDt,
                (targetCenter.y - m_previousTargetPosition.y) / sampleDt,
                (targetCenter.z - m_previousTargetPosition.z) / sampleDt
            };
            const float measuredSpeed = std::sqrt(measuredVelocity.x * measuredVelocity.x +
                measuredVelocity.y * measuredVelocity.y + measuredVelocity.z * measuredVelocity.z);
            if (!finiteVec3(measuredVelocity)) {
                measuredVelocity = {};
            } else if (measuredSpeed > 8.0f) {
                const float scale = 8.0f / measuredSpeed;
                measuredVelocity.x *= scale;
                measuredVelocity.y *= scale;
                measuredVelocity.z *= scale;
            }
            const float velocityAlpha = 1.0f - std::exp(-10.0f * sampleDt);
            m_targetVelocity.x += (measuredVelocity.x - m_targetVelocity.x) * velocityAlpha;
            m_targetVelocity.y += (measuredVelocity.y - m_targetVelocity.y) * velocityAlpha;
            m_targetVelocity.z += (measuredVelocity.z - m_targetVelocity.z) * velocityAlpha;
        } else {
            m_targetVelocity = {};
        }
    }

    m_previousTargetPosition = targetCenter;
    m_previousTargetSample = sampleNow;
    m_lastTarget = bestTarget;
    m_lastTargetBounds = bestTargetBounds;
    m_lastTargetUpdate = sampleNow;

    // Apply the camera delta on the same game tick that supplied the valid player pointer.
    // Avoid calling game-owned camera code from the EGL/render-frame callback; the crash log
    // showed Scudo heap-header corruption after that cross-cadence path was introduced.
    applyAimStep(player);
}


void AimAssistModule::applyAimStep(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;

    if (!enabled || !player || !m_lastTarget || !g_applyLocalPlayerTurnDelta) return;
    const auto now = std::chrono::steady_clock::now();

    auto* rotationComponent = player->rotationComponent();
    if (!rotationComponent || !validBounds(m_lastTargetBounds)) return;

    Vec2 currentRotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(currentRotation.x) || !std::isfinite(currentRotation.y)) return;
    currentRotation.x = std::clamp(currentRotation.x, -89.0f, 89.0f);
    currentRotation.y = wrapDegrees(currentRotation.y);

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
    const HeldWeapon predictionWeapon = heldWeaponInMainHand(player);
    float predictionSeconds = 0.0f;
    switch (predictionWeapon) {
        case HeldWeapon::Sword:
        case HeldWeapon::Axe:
        case HeldWeapon::Mace:
        case HeldWeapon::Spear:
        case HeldWeapon::OtherMelee:
            predictionSeconds = std::clamp(m_meleePredict, 0.10f, 1.0f);
            break;
        case HeldWeapon::Bow:
        case HeldWeapon::Crossbow:
        case HeldWeapon::Trident:
            predictionSeconds = std::clamp(m_rangedPredict, 0.10f, 1.0f);
            break;
        case HeldWeapon::None: default:
            predictionSeconds = 0.0f;
            break;
    }
    Vec3 predictedOffset{
        m_targetVelocity.x * predictionSeconds,
        m_targetVelocity.y * predictionSeconds,
        m_targetVelocity.z * predictionSeconds
    };
    const float predictedDistance = std::sqrt(predictedOffset.x * predictedOffset.x +
        predictedOffset.y * predictedOffset.y + predictedOffset.z * predictedOffset.z);
    if (std::isfinite(predictedDistance) && predictedDistance > 2.5f) {
        const float predictionScale = 2.5f / predictedDistance;
        predictedOffset.x *= predictionScale;
        predictedOffset.y *= predictionScale;
        predictedOffset.z *= predictionScale;
    }
    bounds.min.x += predictedOffset.x; bounds.max.x += predictedOffset.x;
    bounds.min.y += predictedOffset.y; bounds.max.y += predictedOffset.y;
    bounds.min.z += predictedOffset.z; bounds.max.z += predictedOffset.z;

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

    // Hitbox Area scales the actual region in which the aim point can slide.
    const float hitboxScale = std::clamp(m_hitboxScale, 0.5f, 1.0f);
    const Vec3 halfExtents{
        (bounds.max.x - bounds.min.x) * 0.5f * hitboxScale,
        (bounds.max.y - bounds.min.y) * 0.5f * hitboxScale,
        (bounds.max.z - bounds.min.z) * 0.5f * hitboxScale
    };
    const Vec3 clampedSlidePoint{
        std::clamp(projectedPoint.x, boundsCenter.x - halfExtents.x, boundsCenter.x + halfExtents.x),
        std::clamp(projectedPoint.y, boundsCenter.y - halfExtents.y, boundsCenter.y + halfExtents.y),
        std::clamp(projectedPoint.z, boundsCenter.z - halfExtents.z, boundsCenter.z + halfExtents.z)
    };
    // Never let the point fully follow the camera ray: that makes aim error collapse
    // to zero inside the hitbox and then jump when the ray exits its edge. Blending
    // from the stable center preserves a steering signal while allowing visible slide.
    constexpr float kHitboxSlideBlend = 0.65f;
    const Vec3 slidePoint{
        centerAimPoint.x + (clampedSlidePoint.x - centerAimPoint.x) * kHitboxSlideBlend,
        centerAimPoint.y + (clampedSlidePoint.y - centerAimPoint.y) * kHitboxSlideBlend,
        centerAimPoint.z + (clampedSlidePoint.z - centerAimPoint.z) * kHitboxSlideBlend
    };

    float deltaSeconds = 1.0f / 60.0f;
    if (m_lastRotationUpdate.time_since_epoch().count() != 0) {
        deltaSeconds = std::clamp(
            std::chrono::duration<float>(now - m_lastRotationUpdate).count(),
            0.005f, 0.08f);
    }
    m_lastRotationUpdate = now;

    if (!m_hasAimPoint) {
        m_lastAimPoint = centerAimPoint;
        m_hasAimPoint = true;
    }
    const float slideAlpha = 1.0f - std::exp(-18.0f * deltaSeconds);
    m_lastAimPoint.x += (slidePoint.x - m_lastAimPoint.x) * slideAlpha;
    m_lastAimPoint.y += (slidePoint.y - m_lastAimPoint.y) * slideAlpha;
    m_lastAimPoint.z += (slidePoint.z - m_lastAimPoint.z) * slideAlpha;

    const float aimDx = m_lastAimPoint.x - eyePosition.x;
    const float aimDy = m_lastAimPoint.y - eyePosition.y;
    const float aimDz = m_lastAimPoint.z - eyePosition.z;
    const float aimHorizontal = std::sqrt(aimDx * aimDx + aimDz * aimDz);
    const float desiredYaw = std::atan2(-aimDx, aimDz) * kRadToDeg;
    const float desiredPitch = -std::atan2(aimDy, std::max(aimHorizontal, 0.001f)) * kRadToDeg;
    const float yawError = wrapDegrees(desiredYaw - currentRotation.y);
    const float pitchError = desiredPitch - currentRotation.x;
    if (!std::isfinite(yawError) || !std::isfinite(pitchError)) return;

    // A stronger speed curve makes the full 0.05..5.0 range perceptible.
    // The per-frame cap limits visible jumps when FPS drops.
    const float speed = std::clamp(m_strength, 0.05f, 5.0f);
    const float speedScale = std::sqrt(speed);
    const float maxTurnRate = 100.0f + 280.0f * speedScale;
    const float smoothTime = std::clamp(0.12f / (0.6f + 0.7f * speedScale), 0.035f, 0.15f);
    const float yawDelta = smoothDampedAxisStep(
        yawError, deltaSeconds, maxTurnRate, smoothTime, m_yawVelocity);
    const float pitchDelta = smoothDampedAxisStep(
        pitchError, deltaSeconds, maxTurnRate, smoothTime, m_pitchVelocity);
    if (!std::isfinite(yawDelta) || !std::isfinite(pitchDelta)) return;
    if (yawDelta == 0.0f && pitchDelta == 0.0f) return;

    Vec2 turnDelta{-pitchDelta, yawDelta};
    g_applyLocalPlayerTurnDelta(player, &turnDelta);
}

void AimAssistModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    m_targetPlayers = j.value("m_targetPlayers", m_targetPlayers);
    m_targetMobs = j.value("m_targetMobs", m_targetMobs);
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
    m_hitboxScale = std::clamp(j.value("m_hitboxScale", m_hitboxScale), 0.5f, 1.0f);
    m_strength = std::clamp(j.value("m_strength", m_strength), 0.05f, 5.0f);
    m_meleePredict = std::clamp(j.value("m_meleePredict", m_meleePredict), 0.10f, 1.0f);
    m_rangedPredict = std::clamp(j.value("m_rangedPredict", m_rangedPredict), 0.10f, 1.0f);
    m_whitelistNames = j.value("m_whitelistNames", m_whitelistNames);
}

void AimAssistModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    j["m_targetPlayers"] = m_targetPlayers;
    j["m_targetMobs"] = m_targetMobs;
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
    j["m_hitboxScale"] = m_hitboxScale;
    j["m_strength"] = m_strength;
    j["m_meleePredict"] = m_meleePredict;
    j["m_rangedPredict"] = m_rangedPredict;
    j["m_whitelistNames"] = m_whitelistNames;
}
