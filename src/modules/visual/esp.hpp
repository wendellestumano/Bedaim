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
    bool showBoxes = true;
    bool showTracers = true;
    bool showLockInAim = false;
    bool showOffscreenIndicators = false;
    bool animatedIndicators = false;
    float indicatorSize = 48.0f;
    float range = 30.0f;
    std::uint32_t outlineColor = 0xFFFFFFFF;
    std::uint32_t tracerColor = 0xFFFF3348;

private:
    mutable std::mutex m_hudTargetMutex;
    bedrocktools::sdk::Vec3 m_hudLocalPosition{};
    bedrocktools::sdk::Vec2 m_hudLocalRotation{};
    float m_hudViewportAspect = 16.0f / 9.0f;
    bedrocktools::sdk::Vec3 m_offscreenTargetCenter{};
    float m_offscreenTargetDistance = 0.0f;
    bool m_offscreenTargetValid = false;
    std::chrono::steady_clock::time_point m_offscreenFrameTime{};
};

[executed on device: localhost (e1d4d221-df7a-4b7f-8115-0ecb8c62d73c)]