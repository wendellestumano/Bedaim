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

bool bedaimApplySyntheticLocalPlayerTurnDelta(void* localPlayer, float pitchDelta, float yawDelta);

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
    while (angle > 180.0f) angle -= 360.0f;
    while (angle < -180.0f) angle += 360.0f;
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
    m_aimPointTarget = nullptr;
    m_aimOffsetFrom = {0.5f, 0.55f, 0.5f};
    m_aimOffsetTo = m_aimOffsetFrom;
    m_aimOffsetTransitionStart = {};
    m_nextAimOffsetChange = {};
    std::lock_guard lock(m_frameMutex);
    m_cachedPlayer = nullptr;
    m_hasDesiredRotation = false;
    m_lastTargetUpdate = {};
    m_lastFrameUpdate = {};
}

void AimAssistModule::onDisable() {
    m_lastTarget = nullptr;
    m_aimPointTarget = nullptr;
    m_aimOffsetFrom = {0.5f, 0.55f, 0.5f};
    m_aimOffsetTo = m_aimOffsetFrom;
    m_aimOffsetTransitionStart = {};
    m_nextAimOffsetChange = {};
    std::lock_guard lock(m_frameMutex);
    m_cachedPlayer = nullptr;
    m_hasDesiredRotation = false;
    m_lastTargetUpdate = {};
    m_lastFrameUpdate = {};
}

bedrocktools::sdk::Vec3 AimAssistModule::sampleAimOffset() {
    return {0.5f, 0.55f, 0.5f};
}

bedrocktools::sdk::Vec3 AimAssistModule::currentAimOffset(std::chrono::steady_clock::time_point now) const {
    const float duration = std::max(m_aimOffsetTransitionSeconds, 0.01f);
    const float elapsed = std::chrono::duration<float>(now - m_aimOffsetTransitionStart).count();
    float t = std::clamp(elapsed / duration, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    return {
        m_aimOffsetFrom.x + (m_aimOffsetTo.x - m_aimOffsetFrom.x) * t,
        m_aimOffsetFrom.y + (m_aimOffsetTo.y - m_aimOffsetFrom.y) * t,
        m_aimOffsetFrom.z + (m_aimOffsetTo.z - m_aimOffsetFrom.z) * t
    };
}

void AimAssistModule::updateAimOffsetTarget(void* target, std::chrono::steady_clock::time_point now) {
    if (!target) {
        m_aimPointTarget = nullptr;
        return;
    }

    // Keep one stable point near the target's center instead of chasing a moving random point.
    const bedrocktools::sdk::Vec3 stableOffset{0.5f, 0.55f, 0.5f};
    if (m_aimPointTarget != target || m_aimOffsetTransitionStart.time_since_epoch().count() == 0) {
        m_aimPointTarget = target;
        m_aimOffsetFrom = stableOffset;
        m_aimOffsetTo = stableOffset;
        m_aimOffsetTransitionStart = now;
        m_aimOffsetTransitionSeconds = 0.01f;
        m_nextAimOffsetChange = std::chrono::steady_clock::time_point::max();
    }
}

void AimAssistModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    {
        std::lock_guard lock(m_frameMutex);
        m_cachedPlayer = nullptr;
        m_hasDesiredRotation = false;
    }
    if (!enabled || !player || !g_fetchNearbyActors || (!m_targetPlayers && !m_targetMobs)) {
        m_lastTarget = nullptr;
        return;
    }

    auto* rotationComponent = player->rotationComponent();
    auto* level = player->level();
    if (!rotationComponent || !level) {
        m_lastTarget = nullptr;
        return;
    }

    Vec2 currentRotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(currentRotation.x) || !std::isfinite(currentRotation.y)) {
        m_lastTarget = nullptr;
        return;
    }

    const AABB playerBounds = player->bounds();
    const Vec3 playerPosition = player->position();
    if (!finiteVec3(playerPosition)) {
        m_lastTarget = nullptr;
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

    Vec3 extent{m_range, m_range, m_range};
    void* actorManager = field<void*>(level, offsets::Level::mActorManager);
    if (!actorManager) {
        m_lastTarget = nullptr;
        return;
    }

    ActorVector actors = g_fetchNearbyActors(player, &extent, 1);
    if (!actors.begin || !actors.end || actors.end < actors.begin) {
        m_lastTarget = nullptr;
        return;
    }

    const auto targetNow = std::chrono::steady_clock::now();
    void* bestTarget = nullptr;
    AABB bestBounds{};
    Vec2 bestRotation{};
    float bestAngleScore = std::numeric_limits<float>::max();
    const float maxDistanceSq = m_range * m_range;
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

        // Actor is a lightweight SDK wrapper; no ownership or memory is transferred.
        auto* target = reinterpret_cast<Actor*>(candidate);
        const Vec3 targetPosition = target->position();
        const AABB targetBounds = target->bounds();
        if (!finiteVec3(targetPosition) || !validBounds(targetBounds)) continue;

        Vec3 normalizedOffset{0.5f, 0.55f, 0.5f};
        if (candidate == m_lastTarget && candidate == m_aimPointTarget) {
            normalizedOffset = currentAimOffset(targetNow);
        }

        Vec3 aimPoint{};
        aimPoint.x = targetBounds.min.x + (targetBounds.max.x - targetBounds.min.x) * normalizedOffset.x;
        aimPoint.y = targetBounds.min.y + (targetBounds.max.y - targetBounds.min.y) * normalizedOffset.y;
        aimPoint.z = targetBounds.min.z + (targetBounds.max.z - targetBounds.min.z) * normalizedOffset.z;

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

        // Prefer the target closest to the crosshair, using distance only as a tiebreaker.
        const float targetStickiness = candidate == m_lastTarget ? 0.0f : 2.0f;
        const float score = angleScore + std::sqrt(d2) * 0.001f + targetStickiness;
        if (score < bestAngleScore) {
            bestAngleScore = score;
            bestTarget = candidate;
            bestBounds = targetBounds;
            bestRotation = {desiredPitch, desiredYaw};
        }
    }

    if (!bestTarget) {
        m_lastTarget = nullptr;
        m_aimPointTarget = nullptr;
        return;
    }

    m_lastTarget = bestTarget;
    updateAimOffsetTarget(bestTarget, targetNow);
    const Vec3 normalizedOffset = currentAimOffset(targetNow);
    const Vec3 aimPoint{
        bestBounds.min.x + (bestBounds.max.x - bestBounds.min.x) * normalizedOffset.x,
        bestBounds.min.y + (bestBounds.max.y - bestBounds.min.y) * normalizedOffset.y,
        bestBounds.min.z + (bestBounds.max.z - bestBounds.min.z) * normalizedOffset.z
    };
    const float finalDx = aimPoint.x - eyePosition.x;
    const float finalDy = aimPoint.y - eyePosition.y;
    const float finalDz = aimPoint.z - eyePosition.z;
    const float finalHorizontal = std::sqrt(finalDx * finalDx + finalDz * finalDz);
    bestRotation.y = std::atan2(-finalDx, finalDz) * kRadToDeg;
    bestRotation.x = -std::atan2(finalDy, std::max(finalHorizontal, 0.001f)) * kRadToDeg;

    {
        std::lock_guard lock(m_frameMutex);
        m_cachedPlayer = player;
        m_desiredRotation = bestRotation;
        m_lastTargetUpdate = std::chrono::steady_clock::now();
        m_hasDesiredRotation = true;
    }
}

void AimAssistModule::onFrame() {
    using namespace bedrocktools::sdk;

    Player* player = nullptr;
    Vec2 desiredRotation{};
    float alpha = 0.0f;
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard lock(m_frameMutex);
        if (!enabled) return;

        if (!m_hasDesiredRotation || !m_cachedPlayer) {
            m_lastFrameUpdate = now;
            return;
        }

        // Never dereference an old player pointer if ticks stop during a world transition.
        if (now - m_lastTargetUpdate > std::chrono::milliseconds(150)) {
            m_cachedPlayer = nullptr;
            m_hasDesiredRotation = false;
            m_lastFrameUpdate = now;
            return;
        }

        player = m_cachedPlayer;
        desiredRotation = m_desiredRotation;

        if (m_lastFrameUpdate.time_since_epoch().count() == 0) {
            m_lastFrameUpdate = now;
        }
        const float frameDelta = std::clamp(
            std::chrono::duration<float>(now - m_lastFrameUpdate).count(), 0.0f, 0.05f);
        m_lastFrameUpdate = now;

        // Preserve the configured per-tick strength, but distribute it over rendered frames.
        const float strength = std::clamp(m_strength, 0.01f, 0.95f);
        alpha = 1.0f - std::pow(1.0f - strength, frameDelta * 20.0f);
        alpha = std::clamp(alpha, 0.0f, 0.20f);
    }

    if (!player || alpha <= 0.0001f) return;

    auto* rotationComponent = player->rotationComponent();
    if (!rotationComponent) return;

    const Vec2 currentRotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(currentRotation.x) || !std::isfinite(currentRotation.y)) return;

    const float yawError = wrapDegrees(desiredRotation.y - currentRotation.y);
    const float pitchError = desiredRotation.x - currentRotation.x;
    float yawDelta = std::clamp(yawError * alpha, -3.0f, 3.0f);
    float pitchDelta = std::clamp(pitchError * alpha, -3.0f, 3.0f);

    // Ignore sub-pixel-sized corrections to prevent tiny oscillations around the target.
    if (std::abs(yawDelta) < 0.01f) yawDelta = 0.0f;
    if (std::abs(pitchDelta) < 0.01f) pitchDelta = 0.0f;
    if (yawDelta == 0.0f && pitchDelta == 0.0f) return;

    bedaimApplySyntheticLocalPlayerTurnDelta(player, pitchDelta, yawDelta);
}

void AimAssistModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_targetPlayers = j.value("m_targetPlayers", m_targetPlayers);
    m_targetMobs = j.value("m_targetMobs", m_targetMobs);
    m_range = std::clamp(j.value("m_range", m_range), 2.0f, 80.0f);
    m_fov = std::clamp(j.value("m_fov", m_fov), 1.0f, 360.0f);
    m_strength = std::clamp(j.value("m_strength", m_strength), 0.01f, 1.0f);
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
