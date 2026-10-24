#pragma once
#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <chrono>
#include <cstdint>
#include <mutex>
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

    // Render hook publishes only copied world-space values, never actor pointers.
    void beginOffscreenFrame(const bedrocktools::sdk::Vec3& localPosition,
                             const bedrocktools::sdk::Vec2& localRotation);
    void considerOffscreenCandidate(const bedrocktools::sdk::Vec3& targetCenter,
                                    float distance);
    bool offscreenTargetSnapshot(bedrocktools::sdk::Vec3& targetCenter,
                                 bedrocktools::sdk::Vec3& localPosition,
                                 bedrocktools::sdk::Vec2& localRotation,
                                 float& distance) const;

    bool showPlayers = true;
    bool showInvisible = false;
    bool showLockInAim = true;
    bool showOffscreenIndicators = true;
    bool animatedIndicators = true;
    float indicatorSize = 48.0f;
    float range = 30.0f;
    std::uint32_t survivalColor = 0xFFFFFFFF;
    std::uint32_t creativeColor = 0xFFFF3030;
    std::uint32_t spectatorColor = 0xFFB040FF;
    std::uint32_t adventureColor = 0xFF40D8FF;
    std::uint32_t unknownColor = 0xFFFFD040;

private:
    mutable std::mutex m_hudTargetMutex;
    bedrocktools::sdk::Vec3 m_hudLocalPosition{};
    bedrocktools::sdk::Vec2 m_hudLocalRotation{};
    bedrocktools::sdk::Vec3 m_offscreenTargetCenter{};
    float m_offscreenTargetDistance = 0.0f;
    bool m_offscreenTargetValid = false;
    std::chrono::steady_clock::time_point m_offscreenFrameTime{};
};
