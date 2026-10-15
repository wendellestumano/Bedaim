#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>
#include <chrono>
#include <mutex>

namespace bedrocktools::sdk { class Player; }

class MaceSwapperModule final : public Module {
public:
    MaceSwapperModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Explicit child toggle, in addition to the module's normal master toggle.
    bool m_enableMaceSwapper = true;
    float m_attackRange = 3.0f;
    float m_minimumFallDistance = 1.55f;
    float m_attackIntervalMs = 180.0f;
    bool m_restoreSlot = true;

private:
    void processPlayerTick(bedrocktools::sdk::Player* player);

    std::mutex m_stateMutex;
    float m_lastPlayerY = 0.0f;
    bool m_hasLastPlayerY = false;
    float m_peakY = 0.0f;
    bool m_hasPeakY = false;
    int m_descentTicks = 0;
    int m_stableTicks = 0;
    bool m_fallPriorityActive = false;
    bool m_ownsMaceSlot = false;
    bool m_userOverrodeSlot = false;
    bool m_waitingAfterEquip = false;
    int m_fallMaceSlot = -1;
    int m_fallPreviousSlot = -1;
    std::chrono::steady_clock::time_point m_nextAttack{};
};
