#pragma once

#include "../Module.hpp"

#include <atomic>
#include <chrono>
#include <mutex>

namespace bedrocktools::sdk {
class Player;
}

class AutoTotemModule final : public Module {
public:
    AutoTotemModule();

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

private:
    void processPlayerTick(bedrocktools::sdk::Player* player);

    mutable std::mutex m_stateMutex;
    std::atomic_bool m_containerOpen{false};
    bool m_pendingRefill = false;
    bool m_lastOffhandHadTotem = false;
    std::chrono::steady_clock::time_point m_refillAfter{};
    std::chrono::steady_clock::time_point m_nextInventoryScan{};
    std::chrono::steady_clock::time_point m_nextOffhandCheck{};
    int m_cachedTotemSlot = -1;

    float m_delayMs = 100.0f;
    float m_scanRetryIntervalMs = 250.0f;
    float m_offhandPollIntervalMs = 100.0f;
    bool m_combatOnly = false;
    bool m_keepTotemEquipped = true;
};
