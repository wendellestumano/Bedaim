#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>
#include <chrono>
#include <mutex>
#include <string_view>

namespace bedrocktools::sdk { class Player; }

class MaceSwapperModule final : public Module {
public:
    MaceSwapperModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;

    bool m_enableMaceSwapper = true;
    float m_attackRange = 3.0f;
    float m_minimumFallDistance = 1.55f;
    float m_attackIntervalMs = 180.0f;
    bool m_restoreSlot = true;
    bool m_unglideEnabled = false;
    float m_unglideTargetDistance = 8.0f;

private:
    void processPlayerTick(bedrocktools::sdk::Player* player);

    std::mutex m_stateMutex;
    float m_lastPlayerY = 0.0f;
    bool m_hasLastPlayerY = false;
    float m_peakY = 0.0f;
    bool m_hasPeakY = false;
    std::chrono::steady_clock::time_point m_nextAttack{};
    std::chrono::steady_clock::time_point m_nextUnglideAttempt{};
    std::chrono::steady_clock::time_point m_nextUnglideDiagnostic{};
    bool m_pendingAttack = false;
    bool m_cancelPendingAttack = false;
    int m_pendingMaceSlot = -1;
    int m_pendingPreviousSlot = -1;
    void* m_pendingTarget = nullptr;
    bedrocktools::sdk::Player* m_pendingPlayer = nullptr;
};
