#pragma once

#include "../Module.hpp"
#include <mutex>
#include <string>
#include <vector>

class PlayerRadarModule final : public Module {
public:
    PlayerRadarModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

private:
    struct Entry {
        std::string name;
        float distance = 0.0f;
        std::string gameMode = "Unknown";
    };
    mutable std::mutex m_mutex;
    std::vector<Entry> m_players;
    float m_range = 80.0f;
    float m_textSize = 16.0f;
    float m_backgroundOpacity = 0.55f;
    int m_maxPlayers = 8;
    bool m_background = true;
    float hudPosX = 24.0f;
    float hudPosY = 100.0f;
};
