#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>
#include <chrono>
#include <mutex>
#include <random>
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
    void onFrame() override;

    bool m_targetPlayers = true;
    bool m_targetMobs = true;
    float m_range = 5.5f;
    float m_fov = 35.0f;
    float m_strength = 0.12f;
    std::string m_whitelistNames = "";

private:
    void* m_lastTarget = nullptr;
    void* m_aimPointTarget = nullptr;
    bedrocktools::sdk::Vec3 m_aimOffsetFrom{0.5f, 0.55f, 0.5f};
    bedrocktools::sdk::Vec3 m_aimOffsetTo{0.5f, 0.55f, 0.5f};
    std::chrono::steady_clock::time_point m_aimOffsetTransitionStart{};
    std::chrono::steady_clock::time_point m_nextAimOffsetChange{};
    float m_aimOffsetTransitionSeconds = 0.42f;
    std::mt19937 m_rng{std::random_device{}()};

    bedrocktools::sdk::Vec3 sampleAimOffset();
    bedrocktools::sdk::Vec3 currentAimOffset(std::chrono::steady_clock::time_point now) const;
    void updateAimOffsetTarget(void* target, std::chrono::steady_clock::time_point now);
    bedrocktools::sdk::Player* m_cachedPlayer = nullptr;
    bedrocktools::sdk::Vec2 m_desiredRotation{};
    bool m_hasDesiredRotation = false;
    std::chrono::steady_clock::time_point m_lastTargetUpdate{};
    std::chrono::steady_clock::time_point m_lastFrameUpdate{};
    std::mutex m_frameMutex;
};
