#include "aimassist.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Memory.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <string>
#include <limits>


namespace {
using ActorIsPlayerFn = bool (*)(void*);
using ActorIsInvisibleFn = bool (*)(void*);
using ActorGetNameTagFn = std::string (*)(void*);

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
}

AimAssistModule::AimAssistModule()
    : Module("Aim Assist", "Smooth target tracking for private PvP training worlds.") {
    keybind = 0;
    g_aimAssist = this;
}

AimAssistModule::~AimAssistModule() {
    if (g_aimAssist == this) g_aimAssist = nullptr;
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

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_aimAssist && g_aimAssist->enabled) {
                g_aimAssist->processPlayerTick(event.player);
            }
        });
}

void AimAssistModule::onEnable() {
    m_lastTarget = nullptr;
    m_lastRotationUpdate = {};
}

void AimAssistModule::onDisable() {
    m_lastTarget = nullptr;
    m_lastRotationUpdate = {};
}







void AimAssistModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;

    if (!enabled || !player || !g_fetchNearbyActors || (!m_targetPlayers && !m_targetMobs)) {
        m_lastTarget = nullptr;
        m_lastRotationUpdate = {};
        return;
    }

    auto* rotationComponent = player->rotationComponent();
    auto* level = player->level();
    if (!rotationComponent || !level) {
        m_lastTarget = nullptr;
        m_lastRotationUpdate = {};
        return;
    }

    Vec2 currentRotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(currentRotation.x) || !std::isfinite(currentRotation.y)) {
        m_lastTarget = nullptr;
        m_lastRotationUpdate = {};
        return;
    }
    currentRotation.x = std::clamp(currentRotation.x, -89.0f, 89.0f);
    currentRotation.y = wrapDegrees(currentRotation.y);

    const AABB playerBounds = player->bounds();
    const Vec3 playerPosition = player->position();
    if (!finiteVec3(playerPosition)) {
        m_lastTarget = nullptr;
        m_lastRotationUpdate = {};
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
        m_lastTarget = nullptr;
        m_lastRotationUpdate = {};
        return;
    }

    ActorVector actors = g_fetchNearbyActors(player, &extent, 1);
    if (!actors.begin || !actors.end || actors.end < actors.begin) {
        m_lastTarget = nullptr;
        m_lastRotationUpdate = {};
        return;
    }

    void* bestTarget = nullptr;
    Vec2 bestRotation{};
    float bestScore = std::numeric_limits<float>::max();
    const float maxDistanceSq = range * range;
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

        // Aim at a fixed point in the target's hitbox; never chase a randomized offset.
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

        const float yawError = wrapDegrees(desiredYaw - currentRotation.y);
        const float pitchError = desiredPitch - currentRotation.x;
        const float angleScore = std::sqrt(yawError * yawError + pitchError * pitchError);
        if (!std::isfinite(angleScore) || angleScore > maxFov) continue;

        // Hysteresis: keep the current target unless another one is clearly better.
        const float targetStickiness = candidate == m_lastTarget ? 0.0f : 8.0f;
        const float score = angleScore + std::sqrt(d2) * 0.001f + targetStickiness;
        if (score < bestScore) {
            bestScore = score;
            bestTarget = candidate;
            bestRotation = {desiredPitch, desiredYaw};
        }
    }

    if (!bestTarget) {
        m_lastTarget = nullptr;
        m_lastRotationUpdate = {};
        return;
    }

    m_lastTarget = bestTarget;
    const float yawError = wrapDegrees(bestRotation.y - currentRotation.y);
    const float pitchError = bestRotation.x - currentRotation.x;
    if (!std::isfinite(yawError) || !std::isfinite(pitchError)) return;

    const auto now = std::chrono::steady_clock::now();
    float deltaSeconds = 0.05f;
    if (m_lastRotationUpdate.time_since_epoch().count() != 0) {
        deltaSeconds = std::clamp(
            std::chrono::duration<float>(now - m_lastRotationUpdate).count(), 0.01f, 0.05f);
    }
    m_lastRotationUpdate = now;

    // Strength is a speed control from 0.05 to 5.0, not a raw exponent base.
    // The exponential response stays finite for every supported setting.
    const float speed = std::clamp(m_strength, 0.05f, 5.0f);
    const float alpha = std::clamp(1.0f - std::exp(-speed * 20.0f * deltaSeconds), 0.0f, 0.95f);
    const float maxTurnRate = 240.0f + 72.0f * speed;
    const float maxStep = maxTurnRate * deltaSeconds;

    const float yawDelta = std::clamp(yawError * alpha, -maxStep, maxStep);
    const float pitchDelta = std::clamp(pitchError * alpha, -maxStep, maxStep);
    const Vec2 updatedRotation{
        std::clamp(currentRotation.x + (std::abs(pitchDelta) < 0.002f ? 0.0f : pitchDelta), -89.0f, 89.0f),
        wrapDegrees(currentRotation.y + (std::abs(yawDelta) < 0.002f ? 0.0f : yawDelta))
    };

    if (!std::isfinite(updatedRotation.x) || !std::isfinite(updatedRotation.y)) return;

    // Apply the bounded correction directly to the same rotation component we read.
    // This avoids a synthetic-input path with a separate/stale rotation state.
    field<Vec2>(rotationComponent, 0) = updatedRotation;
}



void AimAssistModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_targetPlayers = j.value("m_targetPlayers", m_targetPlayers);
    m_targetMobs = j.value("m_targetMobs", m_targetMobs);
    m_range = std::clamp(j.value("m_range", m_range), 2.0f, 80.0f);
    m_fov = std::clamp(j.value("m_fov", m_fov), 1.0f, 360.0f);
    m_strength = std::clamp(j.value("m_strength", m_strength), 0.05f, 5.0f);
    m_whitelistNames = j.value("m_whitelistNames", m_whitelistNames);
}

void AimAssistModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_targetPlayers"] = m_targetPlayers;
    j["m_targetMobs"] = m_targetMobs;
    j["m_range"] = m_range;
    j["m_fov"] = m_fov;
    j["m_strength"] = m_strength;
    j["m_whitelistNames"] = m_whitelistNames;
}
