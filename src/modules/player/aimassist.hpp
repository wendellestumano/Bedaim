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

    bool m_targetPlayers = true;
    bool m_targetMobs = true;
    bool m_weaponOnly = false;
    float m_range = 5.5f;
    float m_fov = 35.0f;
    float m_hitboxScale = 1.0f;
    float m_strength = 0.12f;
    std::string m_whitelistNames = "";

private:
    void resetTrackingState();
    std::mutex m_stateMutex;
    void* m_lastTarget = nullptr;
    bedrocktools::sdk::AABB m_lastTargetBounds{};
    bedrocktools::sdk::Vec3 m_lastAimPoint{};
    bool m_hasAimPoint = false;
    float m_yawVelocity = 0.0f;
    float m_pitchVelocity = 0.0f;
    std::chrono::steady_clock::time_point m_lastTargetUpdate{};
    std::chrono::steady_clock::time_point m_lastRotationUpdate{};
};
