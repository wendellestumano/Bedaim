#pragma once

#include "../Module.hpp"
#include <chrono>
#include <mutex>

namespace bedrocktools::sdk { class Player; }

class SwordSwapperModule final : public Module {
public:
    SwordSwapperModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float m_attackRange = 3.0f;
    float m_groundDistance = 0.0f;
    bool m_restoreSlot = true;
    bool m_respectManualSlotChanges = false;

private:
    void processPlayerTick(bedrocktools::sdk::Player* player);

    std::mutex m_stateMutex;
    float m_lastPlayerY = 0.0f;
    bool m_hasLastPlayerY = false;
    float m_lastGroundY = 0.0f;
    bool m_hasGroundY = false;
    int m_groundedTicks = 0;
    bool m_ownsSwap = false;
    bool m_userOverrodeSwap = false;
    bool m_foodUseOverride = false;
    int m_swordSlot = -1;
    int m_previousSlot = -1;
};
