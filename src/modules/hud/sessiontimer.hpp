#pragma once

#include "../Module.hpp"
#include <chrono>
#include <mutex>

class SessionTimerModule final : public Module {
public:
    SessionTimerModule();

    void onEnable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

private:
    mutable std::mutex m_configMutex;
    std::chrono::steady_clock::time_point m_startedAt{};
    float hudPosX = 20.0f;
    float hudPosY = 350.0f;
    float m_size = 40.0f;
    float m_backgroundOpacity = 0.5f;
    bool m_background = true;
    bool m_showHours = true;
};
