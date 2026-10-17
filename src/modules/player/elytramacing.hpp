#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>

class ElytraMacingModule final : public Module {
public:
    ElytraMacingModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    void processPlayerTick(bedrocktools::sdk::Player* player);

    mutable std::mutex m_stateMutex;
    bool m_behaviorEnabled = true;
    bool m_requireGliding = true;
    bool m_equipMace = true;
    bool m_restorePreviousSlot = true;
    bool m_requireCrosshair = true;
    bool m_cameraAssist = false;
    float m_aimSpeed = 8.0f;
    float m_attackRange = 5.5f;
    float m_minimumFallDistance = 0.25f;
    float m_attackIntervalMs = 180.0f;
    float m_targetAimHeight = 0.72f;
    float m_crosshairTolerance = 0.12f;

    bool m_pendingAttack = false;
    bedrocktools::sdk::Player* m_pendingPlayer = nullptr;
    void* m_pendingTarget = nullptr;
    int m_pendingMaceSlot = -1;
    int m_pendingPreviousSlot = -1;
    std::chrono::steady_clock::time_point m_nextAttack{};
    float m_lastPlayerY = 0.0f;
    float m_peakY = 0.0f;
    bool m_hasLastPlayerY = false;
    bool m_hasPeakY = false;
    std::atomic_bool m_gliding{false};
};
