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

    struct ScreenOverlay {
        enum class Kind { Box, Ring };
        Kind kind = Kind::Box;
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        float radius = 0.0f;
        float thickness = 1.5f;
        std::uint32_t color = 0xFFFFFFFFu;
    };
    void beginScreenFrame();
    // Publish a completed set of HUD overlays atomically at the end of each render pass.
    void endScreenFrame();
    void addScreenBox(float x, float y, float w, float h, std::uint32_t color, float thickness);
    void addScreenRing(float x, float y, float radius, std::uint32_t color, float thickness);
    std::vector<ScreenOverlay> screenOverlaySnapshot() const;

    bool showPlayers = true;
    bool showInvisible = false;
    bool showLockInAim = true;
    bool showOffscreenIndicators = true;
    bool animatedIndicators = true;
    bool box2D = false;
    bool ring2D = false;
    float indicatorSize = 48.0f;
    float locatorSize = 48.0f;
    float locatorFov = 90.0f;
    float targetYOffset = 0.0f;
    float targetSpeed = 1.0f;
    float targetThickness = 2.0f;
    float outlineThickness = 0.025f;
    float hitboxThickness = 0.025f;
    float range = 30.0f;
    bool infiniteRange = true;
    std::uint32_t survivalColor = 0xFFFFFFFF;
    std::uint32_t creativeColor = 0xFFFF3030;
    std::uint32_t spectatorColor = 0xFFB040FF;
    std::uint32_t adventureColor = 0xFF40D8FF;
    std::uint32_t unknownColor = 0xFFFFD040;

private:
    mutable std::mutex m_hudTargetMutex;
    bedrocktools::sdk::Vec3 m_hudLocalPosition{};
    bedrocktools::sdk::Vec2 m_hudLocalRotation{};
    float m_hudViewportAspect = 16.0f / 9.0f;
    bedrocktools::sdk::Vec3 m_offscreenTargetCenter{};
    float m_offscreenTargetDistance = 0.0f;
    bool m_offscreenTargetValid = false;
    std::chrono::steady_clock::time_point m_offscreenFrameTime{};
    mutable std::mutex m_screenOverlayMutex;
    std::vector<ScreenOverlay> m_screenOverlays;
    std::vector<ScreenOverlay> m_screenBuildingOverlays;
};
