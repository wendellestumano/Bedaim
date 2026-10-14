#pragma once

#include "../Module.hpp"

#include <atomic>
#include <cstdint>

class UltrakillHudModule : public Module {
public:
    UltrakillHudModule();

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    std::atomic<std::uint32_t> m_actionCount{0};

    float hudPosX = 24.0f;
    float hudPosY = 24.0f;
    bool isHudModule = true;

    bool m_showStatusPanel = true;
    bool m_showReticle = true;
    bool m_showStyleCounter = true;
    bool m_showParryWidget = true;
};
