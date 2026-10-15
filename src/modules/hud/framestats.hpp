#pragma once

#include "../Module.hpp"
#include "FrameStatsWindow.hpp"

#include <chrono>
#include <mutex>

class FrameStatsModule final : public Module {
public:
    FrameStatsModule();
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

private:
    mutable std::mutex m_mutex;
    std::chrono::steady_clock::time_point m_lastFrameAt{};
    bedaim::hud::FrameStatsWindow m_window{5000};
    float hudPosX = 20.0f;
    float hudPosY = 410.0f;
    float m_size = 18.0f;
    float m_backgroundOpacity = 0.55f;
    float m_windowSeconds = 5.0f;
    bool m_background = true;
    bool m_showAverage = true;
    bool m_showOnePercentLow = true;
};
