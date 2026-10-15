#pragma once

#include "../Module.hpp"
#include "DistanceAccumulator.hpp"

#include <mutex>

class DistanceTrackerModule final : public Module {
public:
    DistanceTrackerModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

private:
    mutable std::mutex m_mutex;
    bedaim::hud::DistanceAccumulator m_accumulator;
    float hudPosX = 20.0f;
    float hudPosY = 455.0f;
    float m_size = 18.0f;
    float m_backgroundOpacity = 0.55f;
    float m_maxStepBlocks = 16.0f;
    bool m_use3D = false;
    bool m_background = true;
};
