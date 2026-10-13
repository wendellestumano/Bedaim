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

    bool m_targetPlayers = true;
    bool m_targetMobs = true;
    bool m_cantSeeThroughBlocks = true;
    std::string m_aimMode = "Classic";
    bool m_elytraMode = false;
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
    float m_hitboxScale = 1.0f;
    float m_strength = 0.35f;
    std::string m_whitelistNames = "";

private:
    void resetTrackingState();
    void applyAimStep(bedrocktools::sdk::Player* player);
    std::mutex m_stateMutex;
    void* m_lastTarget = nullptr;
    bedrocktools::sdk::AABB m_lastTargetBounds{};
    bedrocktools::sdk::Vec3 m_lastAimPoint{};
    bool m_hasAimPoint = false;
    bool m_localPlayerGliding = false;
    bedrocktools::sdk::Vec2 m_lastObservedRotation{};
    bool m_hasObservedRotation = false;
    float m_lastAssistYawDelta = 0.0f;
    float m_lastAssistPitchDelta = 0.0f;
    std::chrono::steady_clock::time_point m_lastRotationUpdate{};
};
