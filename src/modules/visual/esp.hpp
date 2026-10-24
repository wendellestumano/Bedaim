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

    struct OffscreenTarget {
        bedrocktools::sdk::Vec3 center{};
        float distance = 0.0f;
        // Identity is copied as an opaque per-frame token; it is never
        // dereferenced by the HUD thread or retained as an actor pointer.
        std::uintptr_t identity = 0;
    };

    // Render hook publishes copied world-space values, never actor pointers.
    void beginOffscreenFrame(const bedrocktools::sdk::Vec3& localPosition,
                             const bedrocktools::sdk::Vec2& localRotation);
    void considerOffscreenCandidate(const bedrocktools::sdk::Vec3& targetCenter,
                                    float distance, std::uintptr_t identity);
    bool offscreenTargetsSnapshot(std::vector<OffscreenTarget>& targets,
                                  bedrocktools::sdk::Vec3& localPosition,
                                  bedrocktools::sdk::Vec2& localRotation) const;

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
    float m_hudViewportAspect = 16.0f / 9.0f;
    static constexpr std::size_t kMaxOffscreenTargets = 24;
    std::vector<OffscreenTarget> m_offscreenTargets;
    std::chrono::steady_clock::time_point m_offscreenFrameTime{};
};