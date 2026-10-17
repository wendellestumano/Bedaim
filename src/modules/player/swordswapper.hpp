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
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float m_attackRange = 3.0f;
    bool m_restoreSlot = true;

private:
    void processPlayerTick(bedrocktools::sdk::Player* player);

    std::mutex m_stateMutex;
    bool m_ownsSwap = false;
    bool m_userOverrodeSwap = false;
    bool m_foodUseOverride = false;
    int m_swordSlot = -1;
    int m_previousSlot = -1;
};
