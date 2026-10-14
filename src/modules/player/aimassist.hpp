#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>
#include <chrono>
#include <mutex>
#include <string>

class AimAssistModule final : public Module {
public:
    AimAssistModule();
    ~AimAssistModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void processPlayerTick(bedrocktools::sdk::Player* player);
    void processPlayerFrame();
    bool currentTargetSnapshot(void*& target, bedrocktools::sdk::AABB& bounds, float& range);
    bool targetBoundsSnapshot(void* desiredTarget, bedrocktools::sdk::AABB& bounds, float& range);
    bool currentTargetInCrosshair(bedrocktools::sdk::Player* player, void* target, float tolerance = 0.12f);
    void deferAutoHitFor(float delayMs);

    bool m_targetPlayers = true;
    bool m_targetMobs = true;
    bool m_cantSeeThroughBlocks = true;
    std::string m_aimMode = "Classic";
    std::string m_trackingMode = "Smooth";
    bool m_elytraMode = false;
    bool m_naturalization = true;
    std::string m_naturalizationWeapon = "All";
    bool m_weaponOnly = false;
    bool m_weaponSword = true;
    bool m_weaponAxe = true;
    bool m_weaponMace = true;
    bool m_weaponBow = true;
    bool m_weaponCrossbow = true;
    bool m_weaponTrident = true;
    bool m_weaponSpear = true;
    bool m_weaponOtherMelee = true;
    float m_range = 5.5f;
    float m_fov = 35.0f;
    float m_targetSwitchHysteresis = 4.0f;
    float m_targetSwitchDistanceMargin = 0.75f;
    float m_targetRetentionRadius = 2.75f;
    std::string m_targetSelectionMode = "Crosshair";
    bool m_autoHit = false;
    float m_autoHitRange = 3.0f;
    float m_autoHitDelayMs = 150.0f;
    float m_hitboxScale = 1.0f;
    bool m_distanceDependentHitbox = true;
    float m_hitboxDistanceGrowth = 5.0f;
    float m_strength = 0.25f;
    float m_aimSpeed = 5.0f;
    // Independent yaw/pitch turn-rate multiplier for large rotations behind the player.
    float m_rotationSpeed = 1.0f;
    // Per-weapon multipliers let ranged tracking stay deliberate while melee can respond faster.
    float m_swordAimMultiplier = 1.0f;
    float m_axeAimMultiplier = 1.0f;
    float m_maceAimMultiplier = 1.0f;
    float m_bowAimMultiplier = 0.85f;
    float m_crossbowAimMultiplier = 0.75f;
    float m_tridentAimMultiplier = 0.9f;
    float m_spearAimMultiplier = 1.0f;
    float m_otherMeleeAimMultiplier = 1.0f;
    std::string m_predictionMode = "Ranged";
    float m_predictionLeadMs = 120.0f;
    std::string m_whitelistNames = "";

private:
    void resetTrackingState();
    void processPlayerTickLocked(bedrocktools::sdk::Player* player);
    void applyAimStepLocked(bedrocktools::sdk::Player* player);
    void processAutoHitLocked(bedrocktools::sdk::Player* player);
    std::mutex m_stateMutex;
    bedrocktools::sdk::Player* m_lastPlayer = nullptr;
    void* m_lastTarget = nullptr;
    bool m_targetPresent = false;
    bedrocktools::sdk::AABB m_lastTargetBounds{};
    bedrocktools::sdk::Vec3 m_lastAimPoint{};
    bool m_hasAimPoint = false;
    bool m_localPlayerGliding = false;
    float m_lastPlayerY = 0.0f;
    bool m_hasLastPlayerY = false;
    std::chrono::steady_clock::time_point m_pitchHoldUntil{};
    bedrocktools::sdk::Vec2 m_lastObservedRotation{};
    bool m_hasObservedRotation = false;
    float m_lastAssistYawDelta = 0.0f;
    float m_lastAssistPitchDelta = 0.0f;
    std::chrono::steady_clock::time_point m_lastRotationUpdate{};
    std::chrono::steady_clock::time_point m_lastTargetSeen{};
    std::chrono::steady_clock::time_point m_nextAutoHit{};
    void* m_motionTrackedTarget = nullptr;
    bedrocktools::sdk::Vec3 m_lastMotionPosition{};
    bedrocktools::sdk::Vec3 m_targetVelocity{};
    bool m_hasMotionSample = false;
    std::chrono::steady_clock::time_point m_lastMotionSample{};
};

// Client-side virtual hitbox expander for Aim Assist. It widens the steering
// region only; it does not rewrite the server-authoritative collision AABB.
class HitboxExpanderModule final : public Module {
public:
    HitboxExpanderModule();
    bedrocktools::sdk::AABB expandedSteeringBounds(bool isPlayer, const bedrocktools::sdk::AABB& nativeBounds) const;
    bool requiresLineOfSight() const;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float m_hitboxSize = 0.6f;
    bool m_expandPlayers = true;
    bool m_expandMobs = false;
    bool m_requireLineOfSight = true;

private:
    mutable std::mutex m_expandMutex;
};
