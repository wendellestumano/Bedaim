#pragma once
#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <cstdint>
#include <string_view>

class EspModule final : public Module {
public:
    EspModule();
    void onInit() override;
    void onEnable() override;
    void onFrame() override;
    void onDisable() override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool showPlayers = true;
    bool showInvisible = false;
    bool showLockInAim = true;
    bool animatedIndicators = true;
    float indicatorSize = 48.0f;
    float range = 30.0f;
    std::uint32_t survivalColor = 0xFFFFFFFF;
    std::uint32_t creativeColor = 0xFFFF3030;
    std::uint32_t spectatorColor = 0xFFB040FF;
    std::uint32_t adventureColor = 0xFF40D8FF;
    std::uint32_t unknownColor = 0xFFFFD040;

private:
};
