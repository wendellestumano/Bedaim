#pragma once
#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

class EspModule final : public Module {
public:
    struct ProjectedBox {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        std::uint32_t color = 0xFFFFFFFFu;
    };
    EspModule();
    void onInit() override;
    void onEnable() override;
    void onFrame() override;
    void publishProjectedBoxes(std::vector<ProjectedBox> boxes);
    void onDisable() override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool showPlayers = true;
    bool showInvisible = false;
    bool showLockInAim = true;
    bool showTracers = true;
    bool animatedIndicators = true;
    float indicatorSize = 48.0f;
    float range = 30.0f;
    std::uint32_t survivalColor = 0xFFFFFFFF;
    std::uint32_t creativeColor = 0xFFFF3030;
    std::uint32_t spectatorColor = 0xFFB040FF;
    std::uint32_t adventureColor = 0xFF40D8FF;
    std::uint32_t unknownColor = 0xFFFFD040;

private:
    mutable std::mutex m_projectedMutex;
    std::vector<ProjectedBox> m_projectedBoxes;
    std::chrono::steady_clock::time_point m_projectedFrameTime{};
};
