#pragma once

#include "../Module.hpp"
#include "SeedMapEngine.hpp"

#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/network/Packet.hpp>
#include <pl/Input.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class SeedMapModule final : public Module {
public:
    SeedMapModule();

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

    void onPlayerTick(bedrocktools::sdk::Player* player);
    void observeTextPacket(const bedrocktools::sdk::TextPacket& packet);

private:
    bool installTextHook();
    bool pasteClipboardSeed();
    bool onTouch(const pl::input::TouchEvent& event);

    static std::string parseSeedAnnouncement(std::string_view message);
    static bool parseSeedBits(std::string_view value, std::uint64_t& out);
    static bool isValidSeedNumber(std::string_view value);
    static std::string readClipboardText();

    void refreshMarkers(std::uint64_t seed, bedaim::seedmap::Dimension dimension,
                        int centerX, int centerZ, int radius);
    void drawMap(float screenW, float screenH,
                 const std::vector<bedaim::seedmap::StructureMarker>& markers,
                 int playerX, int playerY, int playerZ, bedaim::seedmap::Dimension dimension,
                 bool hasSeed, int mapRadius, bool bigMap);

    mutable std::mutex m_stateMutex;
    std::string m_seed;
    std::string m_dimensionName{"overworld"};
    bedaim::seedmap::Dimension m_dimension{bedaim::seedmap::Dimension::Overworld};
    int m_playerX = 0;
    int m_playerY = 0;
    int m_playerZ = 0;
    bool m_hasPlayerPosition = false;
    bool m_markersDirty = true;
    std::uint64_t m_cachedSeed = 0;
    int m_cachedCenterX = 0;
    int m_cachedCenterZ = 0;
    int m_cachedRadius = 0;
    bedaim::seedmap::Dimension m_cachedDimension{bedaim::seedmap::Dimension::Overworld};
    std::chrono::steady_clock::time_point m_lastMarkerRefresh{};
    std::vector<bedaim::seedmap::StructureMarker> m_markers;

    bool m_showMap = true;
    bool m_showStructures = true;
    bool m_showChunkGrid = true;
    bool m_showCoordinates = true;
    bool m_bigMap = false;
    float hudPosX = 18.0f;
    float hudPosY = 82.0f;
    float m_mapSize = 230.0f;
    int m_mapRadiusBlocks = 1024;
    int m_panOffsetX = 0;
    int m_panOffsetZ = 0;
    bool m_touchStartedOnMap = false;

    bool m_hasSelectedMarker = false;
    int m_selectedMarkerX = 0;
    int m_selectedMarkerZ = 0;

    bool m_touchTracking = false;
    int m_touchPointerId = -1;
    float m_touchDownX = 0.0f;
    float m_touchDownY = 0.0f;
    float m_touchStartHudX = 0.0f;
    float m_touchStartHudY = 0.0f;
    float m_lastPanelX = 0.0f;
    float m_lastPanelY = 0.0f;
    float m_lastPanelSize = 0.0f;
    float m_lastMapRadius = 0.0f;
    float m_lastMapLeft = 0.0f;
    float m_lastMapTop = 0.0f;
    float m_lastMapWidth = 0.0f;
    float m_lastMapHeight = 0.0f;
    bool m_lastBigMap = false;
};
