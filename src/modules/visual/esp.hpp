#pragma once

#include "../Module.hpp"
#include <cstdint>

class EspModule : public Module {
public:
    EspModule();

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool showPlayers = true;
    bool showInvisible = false;
    float range = 30.0f;

    // ARGB colors. These defaults follow the Apollon-style game-mode palette.
    uint32_t survivalColor = 0xFFFFFFFF;
    uint32_t creativeColor = 0xFFFF3030;
    uint32_t spectatorColor = 0xFFB040FF;
    uint32_t adventureColor = 0xFF40D8FF;
    uint32_t unknownColor = 0xFFFFD040;
};
