#pragma once
#include "../Module.hpp"
#include <cstdint>
#include <string_view>

class BlockEspModule final : public Module {
public:
    BlockEspModule();
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool showPlayers = true;
    bool showInvisible = false;
    bool infiniteRange = true;
    bool box2D = false;
    float range = 30.0f;
    float box2DThickness = 2.0f;
    float projectionFov = 70.0f;
    float outlineThickness = 0.025f;
    std::uint32_t survivalColor = 0xFFFFFFFFu;
    std::uint32_t creativeColor = 0xFFFF3030u;
    std::uint32_t spectatorColor = 0xFFB040FFu;
    std::uint32_t adventureColor = 0xFF40D8FFu;
    std::uint32_t unknownColor = 0xFFFFD040u;
};
