#pragma once

#include "../Module.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>

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
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

private:
    void processPlayerTick(bedrocktools::sdk::Player* player);

    std::mutex m_stateMutex;
    std::atomic_bool m_containerOpen{false};
    bool m_pendingRefill = false;
    bool m_lastOffhandHadTotem = false;
    bool m_waitingForKeymap = false;
    bool m_dispatchInProgress = false;
    bool m_retryBlocked = false;
    bool m_confirmationPending = false;
    int m_dispatchAttempts = 0;
    std::chrono::steady_clock::time_point m_refillAfter{};
    std::chrono::steady_clock::time_point m_nextAttempt{};
    std::chrono::steady_clock::time_point m_confirmationDeadline{};
    float m_delayMs = 100.0f;
    bool m_combatOnly = false;
    bool m_keepTotemEquipped = true;
    std::string m_keymapUid;
};
