#pragma once

#include "../Module.hpp"
#include <mutex>
#include <string>
#include <vector>

struct PlayerModeEntry {
    std::string name;
    std::string mode;
};

struct GamemodeCheckerDiagnostics {
    std::size_t actorCount = 0;
    std::size_t playerCount = 0;
    std::size_t namedPlayerCount = 0;
    std::size_t exactModeCount = 0;
    std::size_t inferredModeCount = 0;
    std::size_t unresolvedModeCount = 0;
    bool actorListReady = false;
    bool playerClassifierReady = false;
    bool nameGetterReady = false;
    bool levelReady = false;
    bool actorManagerReady = false;
};

class GamemodeCheckerModule final : public Module {
public:
    GamemodeCheckerModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    static bool isSpectatorCandidate(void* actor);

private:
    void refresh(void* localPlayer);
    std::mutex m_mutex;
    std::vector<PlayerModeEntry> m_players;
    GamemodeCheckerDiagnostics m_diagnostics;
    unsigned m_ticks = 0;
};
