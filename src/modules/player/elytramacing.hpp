#pragma once

#include "../Module.hpp"

#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace bedrocktools::sdk { class Player; }

class ElytraMacingModule final : public Module {
public:
    ElytraMacingModule();

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

public:
    enum class Phase {
        Chase,
        AwaitingGlide,
        AttackWindow,
        RecoveryLookUp,
        RecoveryGlide
    };

    enum class Action {
        None,
        StartGlide,
        Firework,
        Unglide,
        LookUp
    };

private:
    void processPlayerTick(bedrocktools::sdk::Player* player);
    bool requestKeymapLocked(Action action, const std::string& uid,
                             std::chrono::steady_clock::time_point now);
    void cleanupManagedSlotLocked(bedrocktools::sdk::Player* player);
    bool selectManagedItemLocked(bedrocktools::sdk::Player* player,
                                 std::string_view identifierPart);
    void setAimSuspendedLocked(bool suspended);
    void startRecoveryLookUpLocked(std::chrono::steady_clock::time_point now);
    void startRecoveryGlideLocked(std::chrono::steady_clock::time_point now);
    void note(std::string_view field, std::string_view state,
              std::string_view detail = {}) const;

    mutable std::mutex m_stateMutex;
    Phase m_phase = Phase::Chase;
    Action m_lastCompletedAction = Action::None;
    bool m_lastDispatchSucceeded = false;
    bool m_dispatchInProgress = false;
    std::uint64_t m_dispatchToken = 0;
    std::chrono::steady_clock::time_point m_dispatchCompletedAt{};
    std::chrono::steady_clock::time_point m_phaseStarted{};
    std::chrono::steady_clock::time_point m_nextRocket{};
    std::chrono::steady_clock::time_point m_nextGlideAttempt{};
    std::chrono::steady_clock::time_point m_separationDeadline{};

    bedrocktools::sdk::Player* m_lastPlayer = nullptr;
    void* m_cycleTarget = nullptr;
    float m_lastPlayerY = 0.0f;
    bool m_hasLastPlayerY = false;
    float m_verticalDelta = 0.0f;
    bool m_sawFallDuringAttack = false;
    int m_stationaryTicks = 0;
    int m_glideAttempts = 0;
    int m_recoveryGlideAttempts = 0;
    int m_unglideAttempts = 0;
    bool m_boostImmediatelyAfterGlide = false;
    bool m_waitForSeparation = false;
    bool m_recoveryLookUpSent = false;
    bool m_recoveryGlideRequested = false;
    bool m_aimSuspended = false;
    bool m_cleanupRequested = false;

    bool m_slotOwned = false;
    int m_originalSlot = -1;
    int m_ownedSlot = -1;

    // Key Mapper UID fields are editable in the module's separate settings page.
    std::string m_startGlideKeymapUid;
    std::string m_fireworkKeymapUid;
    std::string m_lookUpKeymapUid;
    std::string m_unglideKeymapUid =
        "e2225852-1df2-4232-bb26-606f11fc8745";

    float m_engageDistance = 4.5f;
    float m_approachAimOffsetY = 2.5f;
    float m_windBurstRiseThreshold = 0.08f;
    int m_rocketIntervalMs = 250;
    int m_recoveryLookUpWaitMs = 300;
    int m_landingStableTicks = 5;
    bool m_restoreSlotOnDisable = true;
    bool m_suspendAimDuringRecovery = true;
};
