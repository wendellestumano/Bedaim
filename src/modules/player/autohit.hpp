#pragma once

#include "../Module.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>

namespace bedrocktools::sdk { class Player; }

class AutoHitModule final : public Module {
public:
    AutoHitModule();

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    bool onMouseEvent(int button, bool isDown) override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

private:
    void processPlayerTick(bedrocktools::sdk::Player* player);

    std::mutex m_stateMutex;
    float m_cps = 8.0f;
    std::chrono::steady_clock::time_point m_nextDispatch{};
    std::atomic_bool m_containerOpen{false};
    std::atomic_bool m_chatOpen{false};
};
