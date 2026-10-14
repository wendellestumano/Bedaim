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

    // These can be enabled independently; both share target/range validation.
    bool m_enableMaceSwapper = true;
    bool m_enableSwordSwapper = false;
    float m_attackRange = 3.0f;
    float m_attackIntervalMs = 180.0f;
    bool m_restoreSlot = true;

private:
    void processPlayerTick(bedrocktools::sdk::Player* player);

    std::mutex m_stateMutex;
    float m_lastPlayerY = 0.0f;
    bool m_hasLastPlayerY = false;
    int m_groundedTicks = 0;
    std::chrono::steady_clock::time_point m_nextAttack{};
    bool m_pendingAttack = false;
    int m_pendingMaceSlot = -1;
    int m_pendingPreviousSlot = -1;
    int m_pendingTicks = 0;
    void* m_pendingTarget = nullptr;
    bedrocktools::sdk::Player* m_pendingPlayer = nullptr;

    // Only restore a sword slot if this module selected it and the user has not
    // manually changed slots since then.
    bool m_swordSwapOwned = false;
    int m_swordSlot = -1;
    int m_swordPreviousSlot = -1;
};
