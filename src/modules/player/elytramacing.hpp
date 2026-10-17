#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
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
    enum class FlightPhase : std::uint8_t {
        Idle, Takeoff, WaitForGlide, Climb, Dive, AwaitRebound, Intercept
    };
    void processPlayerTick(bedrocktools::sdk::Player* player);
    bool requestJumpTap(bedrocktools::sdk::Player* player);
    void releaseJumpTap(bedrocktools::sdk::Player* player);
    void restoreOwnedHotbarSlot(bedrocktools::sdk::Player* player);

    mutable std::mutex m_stateMutex;
    bool m_behaviorEnabled = true;
    bool m_requireGliding = true;
    bool m_equipMace = true;
    bool m_restorePreviousSlot = true;
    bool m_requireCrosshair = true;
    bool m_cameraAssist = true;
    bool m_autoTakeoff = true;
    bool m_useFireworks = true;
    float m_climbHeight = 18.0f;
    float m_aimSpeed = 8.0f;
    float m_attackRange = 5.5f;
    float m_minimumFallDistance = 1.5f;
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

    FlightPhase m_phase = FlightPhase::Idle;
    bool m_reboundCycle = false;
    bool m_rocketFired = false;
    int m_phaseTicks = 0;
    int m_groundStableTicks = 0;
    int m_takeoffStage = 0;
    int m_rocketCooldownTicks = 0;
    int m_jumpTapCooldownTicks = 0;
    int m_exitGlideAttempts = 0;
    int m_attackRetryTicks = 0;
    int m_previousHotbarSlot = -1;
    int m_ownedHotbarSlot = -1;
    bool m_jumpPulseActive = false;
    bool m_savedJumpDown = false;
    bool m_savedJumpCurrent = false;
    bool m_savedJumpWasPressed = false;
    bool m_savedJumpWasReleased = false;
};
