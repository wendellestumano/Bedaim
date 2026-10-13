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


float smoothedAxisStep(float error, float deltaSeconds,
                       float maxTurnRate, float response) {
    const float absError = std::abs(error);
    if (!std::isfinite(error) || !std::isfinite(deltaSeconds) ||
        deltaSeconds <= 0.0f || absError < 0.015f) {
        return 0.0f;
    }

    // First-order response has no stored momentum, so the correction cannot
    // continue accelerating past a target after the angular error shrinks.
    const float alpha = 1.0f - std::exp(-response * deltaSeconds);
    const float maxStep = maxTurnRate * deltaSeconds;
    const float step = std::min(absError * alpha, maxStep);
    return std::copysign(step, error);
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

bool hasWeaponInMainHand(const bedrocktools::sdk::Player* player) {
    if (!player) return false;

    auto** vtable = *reinterpret_cast<void***>(
        const_cast<bedrocktools::sdk::Player*>(player));
    constexpr std::size_t getCarriedItemIndex =
        bedrocktools::sdk::offsets::VTable::PlayerGetCarriedItem;
    if (!vtable || !vtable[getCarriedItemIndex]) return false;

    using GetCarriedItemFn = const void* (*)(void*);
    const void* stack = reinterpret_cast<GetCarriedItemFn>(
        vtable[getCarriedItemIndex])(const_cast<bedrocktools::sdk::Player*>(player));
    if (!stack) return false;

    const auto& inventory = bedrocktools::core::InventoryAccess::get();
    constexpr const char* weaponIdentifiers[] = {
        "_sword", "_axe", "mace", "trident", "spear", "bow",
        "dagger", "katana", "rapier", "scythe", "hammer",
        "halberd", "glaive", "lance"
    };
    for (const char* identifier : weaponIdentifiers) {
        if (inventory.countStack(stack, identifier) > 0) return true;
    }
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
    m_lastTarget = nullptr;
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

    if (!enabled || !player || !g_fetchNearbyActors || (!m_targetPlayers && !m_targetMobs) ||
        (m_weaponOnly && !hasWeaponInMainHand(player))) {
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
    if (!actors.begin || !actors.end || actors.end < actors.begin) {
        resetTrackingState();
        return;
    }

    void* bestTarget = nullptr;
    Vec2 bestRotation{};
    void* retainedTarget = nullptr;
    Vec2 retainedRotation{};
    float bestScore = std::numeric_limits<float>::max();
    const float maxDistanceSq = range * range;
    // FOV is the angular cone around the view direction, not a 0..360 scan.
    // A 90-degree cap keeps the cone in front of the player and excludes targets behind.
    // Keep acquisition in a forward-facing cone. The angle score below
    // compares both yaw and pitch error against the actual camera rotation.
    const float maxFov = std::clamp(m_fov, 1.0f, 90.0f);

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

        // Use a fixed, center-biased aim point. It must not move with the view ray,
        // or the aim can chase the edges of the hitbox and orbit around the target.
        const Vec3 aimPoint{
            (targetBounds.min.x + targetBounds.max.x) * 0.5f,
            targetBounds.min.y + (targetBounds.max.y - targetBounds.min.y) * 0.55f,
            (targetBounds.min.z + targetBounds.max.z) * 0.5f
        };

        const float d2 = distanceSquared(eyePosition, aimPoint);
        if (!std::isfinite(d2) || d2 > maxDistanceSq || d2 < 0.01f) continue;

        const float dx = aimPoint.x - eyePosition.x;
        const float dy = aimPoint.y - eyePosition.y;
        const float dz = aimPoint.z - eyePosition.z;
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

        if (candidate == m_lastTarget) {
            retainedTarget = candidate;
            retainedRotation = {desiredPitch, desiredYaw};
        }

        const float score = angleScore + targetDistance * 0.0001f;
        if (score < bestScore) {
            bestScore = score;
            bestTarget = candidate;
            bestRotation = {desiredPitch, desiredYaw};
        }
    }

    if (retainedTarget) {
        bestTarget = retainedTarget;
        bestRotation = retainedRotation;
    }

    if (!bestTarget) {
        resetTrackingState();
        return;
    }

    const bool targetChanged = bestTarget != m_lastTarget;
    if (targetChanged) m_lastRotationUpdate = {};
    m_lastTarget = bestTarget;

    // Apply on the same player-tick path that acquired the target. This avoids
    // resolving a second local-player pointer on another callback path.
    if (!g_applyLocalPlayerTurnDelta) return;
    const float yawError = wrapDegrees(bestRotation.y - currentRotation.y);
    const float pitchError = bestRotation.x - currentRotation.x;
    if (!std::isfinite(yawError) || !std::isfinite(pitchError)) return;

    const auto now = std::chrono::steady_clock::now();
    float deltaSeconds = 1.0f / 20.0f;
    if (m_lastRotationUpdate.time_since_epoch().count() != 0) {
        deltaSeconds = std::clamp(
            std::chrono::duration<float>(now - m_lastRotationUpdate).count(),
            0.01f, 0.08f);
    }
    m_lastRotationUpdate = now;

    // No accumulated momentum: the step shrinks with the angular error.
    const float speed = std::clamp(m_strength, 0.05f, 0.65f);
    const float speedScale = std::sqrt(speed);
    const float maxTurnRate = 40.0f + 45.0f * speedScale;
    const float response = 2.0f + 2.0f * speedScale;
    const float yawDelta = smoothedAxisStep(yawError, deltaSeconds, maxTurnRate, response);
    const float pitchDelta = smoothedAxisStep(pitchError, deltaSeconds, maxTurnRate, response);
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
    m_range = std::clamp(j.value("m_range", m_range), 2.0f, 80.0f);
    m_fov = std::clamp(j.value("m_fov", m_fov), 1.0f, 90.0f);
    m_hitboxScale = std::clamp(j.value("m_hitboxScale", m_hitboxScale), 0.5f, 1.0f);
    m_strength = std::clamp(j.value("m_strength", m_strength), 0.05f, 0.65f);
    m_whitelistNames = j.value("m_whitelistNames", m_whitelistNames);
}

void AimAssistModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    j["m_targetPlayers"] = m_targetPlayers;
    j["m_targetMobs"] = m_targetMobs;
    j["m_weaponOnly"] = m_weaponOnly;
    j["m_range"] = m_range;
    j["m_fov"] = m_fov;
    j["m_hitboxScale"] = m_hitboxScale;
    j["m_strength"] = m_strength;
    j["m_whitelistNames"] = m_whitelistNames;
}
