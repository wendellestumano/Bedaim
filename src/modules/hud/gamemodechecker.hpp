#pragma once

#include "../Module.hpp"
#include <mutex>
#include <string>
#include <vector>

struct PlayerModeEntry {
    std::string name;
    std::string mode;
};

class GamemodeCheckerModule final : public Module {
public:
    GamemodeCheckerModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

private:
    void refresh(void* localPlayer);
    std::mutex m_mutex;
    std::vector<PlayerModeEntry> m_players;
    unsigned m_ticks = 0;
};
