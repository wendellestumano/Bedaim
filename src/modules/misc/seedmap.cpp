#include "seedmap.hpp"

#include "SeedMapEngine.hpp"
#include "../../core/Diagnostics.hpp"
#include "../../core/JavaVm.hpp"
#include "../../core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using HandleTextFn = void (*)(void*, const bedrocktools::sdk::TextPacket&);

std::atomic<SeedMapModule*> g_seedMapModule{nullptr};
void* g_handleTextOriginal = nullptr;
bedrocktools::hooks::Handle g_handleTextHook = nullptr;
std::mutex g_handleTextHookMutex;

constexpr int kActionDown = 0;
constexpr int kActionUp = 1;
constexpr int kActionMove = 2;
constexpr int kActionCancel = 3;

void handleTextDetour(void* handler, const bedrocktools::sdk::TextPacket& packet) {
    if (auto* module = g_seedMapModule.load(std::memory_order_acquire))
        module->observeTextPacket(packet);
    const auto original = reinterpret_cast<HandleTextFn>(g_handleTextOriginal);
    if (original) original(handler, packet);
}

std::string trimCopy(std::string_view input) {
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
        input.remove_prefix(1);
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
        input.remove_suffix(1);
    return std::string(input);
}

void clearJniException(JNIEnv* env) {
    if (env && env->ExceptionCheck()) env->ExceptionClear();
}

bool parseBoolValue(std::string_view value, bool& out) {
    if (value == "true" || value == "1" || value == "on") { out = true; return true; }
    if (value == "false" || value == "0" || value == "off") { out = false; return true; }
    return false;
}

bool parseFloatValue(std::string_view value, float& out) {
    try {
        const std::string text(value);
        std::size_t used = 0;
        const float parsed = std::stof(text, &used);
        while (used < text.size() && std::isspace(static_cast<unsigned char>(text[used]))) ++used;
        if (used != text.size() || !std::isfinite(parsed)) return false;
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseIntValue(std::string_view value, int& out) {
    int parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
    out = parsed;
    return true;
}

int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    const int remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) --quotient;
    return quotient;
}

std::uint32_t markerColor(bedaim::seedmap::StructureType type) {
    using bedaim::seedmap::StructureType;
    switch (type) {
        case StructureType::Village: return 0xFF65D69A;
        case StructureType::Mansion: return 0xFFFF7777;
        case StructureType::EndCity: return 0xFFC5A0FF;
        case StructureType::Monument: return 0xFF5BC8E8;
        case StructureType::AncientCity: return 0xFFAD7BFF;
        case StructureType::Outpost: return 0xFFFFA64D;
        case StructureType::BuriedTreasure: return 0xFFFFD76A;
        case StructureType::OceanRuin: return 0xFF6DA9DC;
        case StructureType::Shipwreck: return 0xFF65C4C7;
        case StructureType::Fortress: return 0xFFE46B6B;
        case StructureType::Bastion: return 0xFFE89A55;
        case StructureType::DesertPyramid:
        case StructureType::Igloo:
        case StructureType::SwampHut:
        case StructureType::JungleTemple: return 0xFFD9C66B;
        case StructureType::RuinedPortal:
        case StructureType::NetherRuinedPortal: return 0xFFD78AFF;
    }
    return 0xFFFFFFFF;
}

std::string markerLabel(const bedaim::seedmap::StructureMarker& marker) {
    if (marker.possibleTypes.size() > 1) return "?";
    return std::string(bedaim::seedmap::structureShortName(marker.possibleTypes.front()));
}

std::string markerDescription(const bedaim::seedmap::StructureMarker& marker) {
    std::string result;
    for (const auto type : marker.possibleTypes) {
        if (!result.empty()) result += " / ";
        result += bedaim::seedmap::structureName(type);
    }
    return marker.possibleTypes.size() > 1 ? "SHARED GRID: " + result : "POSSIBLE: " + result;
}

} // namespace

SeedMapModule::SeedMapModule()
    : Module("SeedMap",
        "In-game seed map overlay inspired by LeviMap. Follows the player and plots Bedrock structure-generation attempts.") {
    showInMenu = true;
}

void SeedMapModule::onInit() {
    g_seedMapModule.store(this, std::memory_order_release);
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { onPlayerTick(event.player); });

    pl::input::registerTouchCallback([](const pl::input::TouchEvent& event) {
        auto* module = g_seedMapModule.load(std::memory_order_acquire);
        return module ? module->onTouch(event) : false;
    });

    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "SeedMap",
        "overlay=registered; input=touch_callback; engine=bedrock_region_rng");
}

void SeedMapModule::onEnable() {
    if (installTextHook()) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "SeedMap",
            "seed_capture=ready; waiting for explicit seed announcement in incoming chat");
    } else {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning, "SeedMap",
            "seed_capture=unavailable; clipboard/manual seed input remains available");
    }
    std::lock_guard lock(m_stateMutex);
    m_markersDirty = true;
}

void SeedMapModule::onDisable() {
    {
        std::lock_guard lock(m_stateMutex);
        m_bigMap = false;
        m_touchTracking = false;
        m_hasSelectedMarker = false;
    }
    const std::vector<pl::modmenu::DrawCommand> empty;
    pl::modmenu::submitDrawCommands(moduleId, empty);
}

bool SeedMapModule::installTextHook() {
    std::lock_guard<std::mutex> lock(g_handleTextHookMutex);
    if (g_handleTextHook) return true;

    const auto target = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::ClientNetworkHandlerHandleText);
    if (!target) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "SeedMap", "chat_seed_hook", "signature_unresolved");
        return false;
    }

    g_handleTextHook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(&handleTextDetour),
        &g_handleTextOriginal);
    if (!g_handleTextHook) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Error,
            "SeedMap", "chat_seed_hook", "install_failed");
        return false;
    }
    return true;
}

bool SeedMapModule::parseSeedBits(std::string_view value, std::uint64_t& out) {
    value = std::string_view(value.data(), value.size());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    if (value.empty()) return false;

    if (value.front() == '-') {
        std::int64_t signedValue = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), signedValue);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return false;
        out = static_cast<std::uint64_t>(signedValue);
        return true;
    }
    if (value.front() == '+') value.remove_prefix(1);
    if (value.empty()) return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

bool SeedMapModule::isValidSeedNumber(std::string_view value) {
    std::uint64_t ignored = 0;
    return parseSeedBits(value, ignored);
}

std::string SeedMapModule::parseSeedAnnouncement(std::string_view message) {
    std::string lower(message);
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const auto marker = lower.find("seed");
    if (marker == std::string::npos) return {};

    std::size_t cursor = marker + 4;
    while (cursor < message.size() &&
        !(std::isdigit(static_cast<unsigned char>(message[cursor])) ||
          message[cursor] == '-' || message[cursor] == '+')) ++cursor;
    if (cursor >= message.size()) return {};

    const std::size_t start = cursor;
    if (message[cursor] == '-' || message[cursor] == '+') ++cursor;
    const std::size_t digitsStart = cursor;
    while (cursor < message.size() &&
           std::isdigit(static_cast<unsigned char>(message[cursor]))) ++cursor;
    if (cursor == digitsStart) return {};

    const auto candidate = message.substr(start, cursor - start);
    return isValidSeedNumber(candidate) ? std::string(candidate) : std::string{};
}

void SeedMapModule::observeTextPacket(const bedrocktools::sdk::TextPacket& packet) {
    if (!enabled) return;
    std::string detected;
    try {
        detected = parseSeedAnnouncement(packet.message());
    } catch (...) {
        return;
    }
    if (detected.empty()) return;

    bool changed = false;
    {
        std::lock_guard lock(m_stateMutex);
        if (m_seed != detected) {
            m_seed = detected;
            m_markersDirty = true;
            m_hasSelectedMarker = false;
            changed = true;
        }
    }
    if (changed) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,
            "SeedMap", "seed_capture=success; numeric world seed received from chat");
    }
}

void SeedMapModule::onPlayerTick(bedrocktools::sdk::Player* player) {
    if (!player) return;
    const auto position = player->position();
    if (!std::isfinite(position.x) || !std::isfinite(position.z)) return;

    const auto toBlock = [](float value) {
        const double floored = std::floor(static_cast<double>(value));
        return static_cast<int>(std::clamp(floored,
            static_cast<double>(std::numeric_limits<int>::min()),
            static_cast<double>(std::numeric_limits<int>::max())));
    };
    const int x = toBlock(position.x);
    const int y = toBlock(position.y);
    const int z = toBlock(position.z);

    int dimensionId = -1;
    if (auto* dimension = player->dimension()) {
        if (auto* source = dimension->blockSource())
            dimensionId = source->dimensionId();
    }

    std::lock_guard lock(m_stateMutex);
    const auto oldDimension = m_dimension;
    if (dimensionId == 0) {
        m_dimension = bedaim::seedmap::Dimension::Overworld;
        m_dimensionName = "OVERWORLD";
    } else if (dimensionId == 1) {
        m_dimension = bedaim::seedmap::Dimension::Nether;
        m_dimensionName = "NETHER";
    } else if (dimensionId == 2) {
        m_dimension = bedaim::seedmap::Dimension::End;
        m_dimensionName = "THE END";
    }

    if (!m_hasPlayerPosition || oldDimension != m_dimension ||
        std::abs(x - m_playerX) >= 64 || std::abs(z - m_playerZ) >= 64) {
        m_markersDirty = true;
    }
    m_playerX = x;
    m_playerY = y;
    m_playerZ = z;
    m_hasPlayerPosition = true;
}

void SeedMapModule::refreshMarkers(std::uint64_t seed,
    bedaim::seedmap::Dimension dimension, int centerX, int centerZ, int radius) {
    auto markers = bedaim::seedmap::findStructureAttempts(
        seed, dimension, centerX, centerZ, radius, 256);
    std::lock_guard lock(m_stateMutex);
    m_markers = std::move(markers);
    m_cachedSeed = seed;
    m_cachedCenterX = centerX;
    m_cachedCenterZ = centerZ;
    m_cachedDimension = dimension;
    m_cachedRadius = radius;
    m_lastMarkerRefresh = std::chrono::steady_clock::now();
    m_markersDirty = false;
    if (m_hasSelectedMarker) {
        const auto selected = std::find_if(m_markers.begin(), m_markers.end(),
            [this](const auto& marker) {
                return marker.x == m_selectedMarkerX && marker.z == m_selectedMarkerZ;
            });
        if (selected == m_markers.end()) m_hasSelectedMarker = false;
    }
}

void SeedMapModule::onFrame() {
    if (!enabled) return;

    std::string seedText;
    int playerX = 0;
    int playerY = 0;
    int playerZ = 0;
    int panOffsetX = 0;
    int panOffsetZ = 0;
    int radius = 1024;
    bool hasPosition = false;
    bool bigMap = false;
    bool dirty = false;
    bool showMap = true;
    bedaim::seedmap::Dimension dimension = bedaim::seedmap::Dimension::Overworld;
    {
        std::lock_guard lock(m_stateMutex);
        seedText = m_seed;
        playerX = m_playerX;
        playerY = m_playerY;
        playerZ = m_playerZ;
        panOffsetX = m_panOffsetX;
        panOffsetZ = m_panOffsetZ;
        hasPosition = m_hasPlayerPosition;
        dimension = m_dimension;
        bigMap = m_bigMap;
        showMap = m_showMap;
        radius = std::clamp(m_mapRadiusBlocks, 256, 8192) * (bigMap ? 2 : 1);
        std::uint64_t parsedSeed = 0;
        const bool hasSeed = parseSeedBits(seedText, parsedSeed);
        const auto now = std::chrono::steady_clock::now();
        dirty = hasSeed && hasPosition &&
            (m_markersDirty || parsedSeed != m_cachedSeed ||
             dimension != m_cachedDimension || radius != m_cachedRadius ||
             std::abs((playerX + panOffsetX) - m_cachedCenterX) >= 64 ||
             std::abs((playerZ + panOffsetZ) - m_cachedCenterZ) >= 64 ||
             m_lastMarkerRefresh.time_since_epoch().count() == 0 ||
             now - m_lastMarkerRefresh > std::chrono::seconds(2));
    }

    if (!showMap) {
        const std::vector<pl::modmenu::DrawCommand> empty;
        pl::modmenu::submitDrawCommands(moduleId, empty);
        return;
    }

    std::uint64_t seed = 0;
    const bool hasSeed = parseSeedBits(seedText, seed);
    if (dirty) refreshMarkers(seed, dimension, playerX + panOffsetX, playerZ + panOffsetZ, radius);

    std::vector<bedaim::seedmap::StructureMarker> markers;
    {
        std::lock_guard lock(m_stateMutex);
        markers = m_markers;
    }

    const auto surface = pl::modmenu::getHudSurfaceSize();
    if (!(surface.width > 0.0f && surface.height > 0.0f)) return;
    drawMap(surface.width, surface.height, markers, playerX, playerY, playerZ,
        dimension, hasSeed && hasPosition, radius, bigMap);
}

void SeedMapModule::drawMap(float screenW, float screenH,
    const std::vector<bedaim::seedmap::StructureMarker>& markers,
    int playerX, int playerY, int playerZ, bedaim::seedmap::Dimension dimension,
    bool hasSeed, int mapRadius, bool bigMap) {

    float panelX = 0.0f;
    float panelY = 0.0f;
    float panelSize = 0.0f;
    bool showStructures = true;
    bool showChunkGrid = true;
    bool showCoordinates = true;
    std::string dimensionName;
    std::string seedText;
    bool hasPosition = false;
    bool selectedValid = false;
    int panOffsetX = 0;
    int panOffsetZ = 0;
    int selectedX = 0;
    int selectedZ = 0;
    {
        std::lock_guard lock(m_stateMutex);
        panelSize = bigMap
            ? std::min(screenW * 0.96f, 980.0f)
            : std::clamp(m_mapSize, 160.0f, 420.0f);
        const float panelHeight = bigMap
            ? std::min(screenH * 0.92f, 1400.0f)
            : panelSize;
        panelX = bigMap ? (screenW - panelSize) * 0.5f
                        : std::clamp(hudPosX, 4.0f, std::max(4.0f, screenW - panelSize - 4.0f));
        panelY = bigMap ? (screenH - panelHeight) * 0.5f
                        : std::clamp(hudPosY, 4.0f, std::max(4.0f, screenH - panelHeight - 4.0f));
        showStructures = m_showStructures;
        showChunkGrid = m_showChunkGrid;
        showCoordinates = m_showCoordinates;
        dimensionName = m_dimensionName;
        seedText = m_seed;
        hasPosition = m_hasPlayerPosition;
        selectedValid = m_hasSelectedMarker;
        panOffsetX = m_panOffsetX;
        panOffsetZ = m_panOffsetZ;
        selectedX = m_selectedMarkerX;
        selectedZ = m_selectedMarkerZ;
        m_lastPanelX = panelX;
        m_lastPanelY = panelY;
        m_lastPanelSize = panelSize;
        m_lastPanelHeight = bigMap
            ? std::min(screenH * 0.92f, 1400.0f)
            : panelSize;
        m_lastMapRadius = static_cast<float>(mapRadius);
        m_lastMapLeft = panelX + 6.0f;
        m_lastMapTop = panelY + (bigMap
            ? std::clamp(std::min(screenH * 0.92f, 1400.0f) * 0.075f, 52.0f, 76.0f)
            : std::clamp(panelSize * 0.19f, 40.0f, 54.0f));
        m_lastMapWidth = panelSize - 12.0f;
        m_lastMapHeight = panelHeight - headerH - footerH;
        m_lastBigMap = bigMap;
    }

    const float panelHeight = bigMap
        ? std::min(screenH * 0.92f, 1400.0f)
        : panelSize;
    const float headerH = bigMap
        ? std::clamp(panelHeight * 0.075f, 52.0f, 76.0f)
        : std::clamp(panelSize * 0.19f, 40.0f, 54.0f);
    const float footerH = bigMap
        ? std::clamp(panelHeight * 0.08f, 52.0f, 76.0f)
        : std::clamp(panelSize * 0.19f, 34.0f, 58.0f);
    const float mapTop = panelY + headerH;
    const float mapBottom = panelY + panelHeight - footerH;
    const float mapHeight = std::max(1.0f, mapBottom - mapTop);
    const float mapWidth = std::max(1.0f, panelSize - 12.0f);
    const float mapLeft = panelX + 6.0f;
    const float mapRight = mapLeft + mapWidth;
    const float centerX = (mapLeft + mapRight) * 0.5f;
    const float centerY = (mapTop + mapBottom) * 0.5f;
    const float scale = std::min(mapWidth, mapHeight) /
        (2.0f * static_cast<float>(std::max(1, mapRadius)));
    const int viewCenterX = playerX + panOffsetX;
    const int viewCenterZ = playerZ + panOffsetZ;
    const float blocksPerPixel = 1.0f / std::max(scale, 0.00001f);
    const float textScale = std::clamp(panelSize / 260.0f, 0.72f, 1.45f);

    std::vector<pl::modmenu::DrawCommand> commands;
    commands.reserve(256);
    using Type = pl::modmenu::DrawCommandType;

    auto rect = [&](float x, float y, float w, float h, std::uint32_t color,
                    float radius = 0.0f, bool filled = true) {
        pl::modmenu::DrawCommand cmd{};
        cmd.type = filled ? Type::RectFilled : Type::Rect;
        cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
        cmd.x3 = radius;
        cmd.size = filled ? 0.0f : 1.0f;
        cmd.color = color;
        commands.push_back(std::move(cmd));
    };
    auto line = [&](float x, float y, float dx, float dy,
                    float thickness, std::uint32_t color) {
        pl::modmenu::DrawCommand cmd{};
        cmd.type = Type::Line;
        cmd.x = x; cmd.y = y; cmd.w = dx; cmd.h = dy;
        cmd.size = thickness; cmd.color = color;
        commands.push_back(std::move(cmd));
    };
    auto circle = [&](float x, float y, float radius, std::uint32_t color) {
        pl::modmenu::DrawCommand cmd{};
        cmd.type = Type::CircleFilled;
        cmd.x = x; cmd.y = y; cmd.size = radius; cmd.color = color;
        commands.push_back(std::move(cmd));
    };
    auto text = [&](float x, float y, std::string value, float size, std::uint32_t color) {
        pl::modmenu::DrawCommand cmd{};
        cmd.type = Type::Text;
        cmd.x = x; cmd.y = y; cmd.text = std::move(value);
        cmd.size = size; cmd.color = color;
        commands.push_back(std::move(cmd));
    };

    // LeviMap-inspired compact panel: dark translucent shell, thin border,
    // restrained accent, coordinate header and a north-up chunk grid.
    rect(panelX, panelY, panelSize, panelHeight, 0xE810141B, 8.0f, true);
    rect(panelX, panelY, panelSize, panelHeight, 0xFF536273, 8.0f, false);
    rect(panelX + 1.0f, panelY + 1.0f, panelSize - 2.0f, 2.0f,
         0xFF55C6B0, 0.0f, true);
    text(panelX + 11.0f, panelY + 7.0f, "SEEDMAP", 12.0f * textScale, 0xFFF1F4F8);
    text(panelX + 11.0f, panelY + 20.0f, "BEDROCK 26.60 / " + dimensionName,
         7.0f * textScale, 0xFF9EABB9);
    text(panelX + 11.0f, panelY + 30.0f,
         "SEED " + (seedText.empty() ? std::string("NOT SET") : seedText),
         6.2f * textScale, 0xFF8FD8CB);

    if (showCoordinates && hasPosition) {
        text(panelX + panelSize * 0.47f, panelY + 9.0f,
            "X " + std::to_string(playerX) + " Y " + std::to_string(playerY) +
            " Z " + std::to_string(playerZ),
            7.0f * textScale, 0xFFE1E7ED);
    }

    const float buttonSize = std::clamp(panelSize * 0.13f, 24.0f, 34.0f);
    const float actionX = panelX + panelSize - buttonSize - 7.0f;
    const float actionY = panelY + 6.0f;
    rect(actionX, actionY, buttonSize, buttonSize, 0xFF293541, 5.0f, true);
    text(actionX + buttonSize * 0.34f, actionY + buttonSize * 0.20f,
         bigMap ? "X" : "B", 12.0f * textScale, 0xFFDAE5EE);

    // The map surface is clipped by geometry bounds before any marker is drawn.
    rect(mapLeft, mapTop, mapWidth, mapHeight, 0xFF101820, 3.0f, true);
    const int gridStep = mapRadius <= 512 ? 16 : (mapRadius <= 4096 ? 128 : 256);
    if (showChunkGrid) {
        const int minX = viewCenterX - mapRadius;
        const int maxX = viewCenterX + mapRadius;
        const int minZ = viewCenterZ - mapRadius;
        const int maxZ = viewCenterZ + mapRadius;
        const int startX = floorDiv(minX, gridStep) * gridStep;
        const int startZ = floorDiv(minZ, gridStep) * gridStep;
        for (int worldX = startX; worldX <= maxX; worldX += gridStep) {
            const float px = centerX + (worldX - playerX) * scale;
            if (px >= mapLeft && px <= mapRight)
                line(px, mapTop, 0.0f, mapHeight, 0.65f,
                     gridStep == 16 ? 0x383E5967 : 0x604A6875);
        }
        for (int worldZ = startZ; worldZ <= maxZ; worldZ += gridStep) {
            const float py = centerY + (worldZ - playerZ) * scale;
            if (py >= mapTop && py <= mapBottom)
                line(mapLeft, py, mapWidth, 0.0f, 0.65f,
                     gridStep == 16 ? 0x383E5967 : 0x604A6875);
        }
    }

    // Origin axes provide orientation without rotating the player-following map.
    const float originX = centerX + (0 - viewCenterX) * scale;
    const float originY = centerY + (0 - viewCenterZ) * scale;
    if (originX >= mapLeft && originX <= mapRight)
        line(originX, mapTop, 0.0f, mapHeight, 1.0f, 0x704E6D7B);
    if (originY >= mapTop && originY <= mapBottom)
        line(mapLeft, originY, mapWidth, 0.0f, 1.0f, 0x704E6D7B);

    if (hasSeed && showStructures) {
        for (const auto& marker : markers) {
            if (marker.possibleTypes.empty()) continue;
            const float px = centerX + (marker.x - viewCenterX) * scale;
            const float py = centerY + (marker.z - viewCenterZ) * scale;
            if (px < mapLeft + 4.0f || px > mapRight - 4.0f ||
                py < mapTop + 4.0f || py > mapBottom - 4.0f) continue;
            const bool selected = selectedValid &&
                marker.x == selectedX && marker.z == selectedZ;
            const auto color = markerColor(marker.possibleTypes.front());
            const float badge = panelSize >= 300.0f ? 6.0f : 4.8f;
            circle(px, py, selected ? badge + 2.5f : badge + 1.2f,
                   selected ? 0xFFFFFFFFu : 0xFF0B1118u);
            circle(px, py, badge,
                   marker.possibleTypes.size() > 1 ? 0xFFFFD76A : color);
            // Compact, structure-specific glyphs: unlike a generic dot, each
            // marker keeps a stable two-letter identity at every map size.
            const std::string glyph = marker.possibleTypes.size() > 1
                ? "?" : markerLabel(marker);
            text(px - (glyph.size() > 1 ? 4.0f : 2.0f),
                 py - 3.2f, glyph, (panelSize >= 300.0f ? 7.0f : 5.6f) * textScale,
                 0xFF101820);
        }
    }

    // Player marker is fixed at the centre; the map moves beneath it.
    const float playerMarkerX = centerX + (playerX - viewCenterX) * scale;
    const float playerMarkerY = centerY + (playerZ - viewCenterZ) * scale;
    circle(playerMarkerX, playerMarkerY, 8.0f, 0xE80A1016);
    circle(playerMarkerX, playerMarkerY, 5.0f, 0xFFFFFFFF);
    circle(playerMarkerX, playerMarkerY, 2.6f, 0xFF55C6B0);
    line(playerMarkerX - 10.0f, playerMarkerY, 20.0f, 0.0f, 1.0f, 0xAA55C6B0);
    line(playerMarkerX, playerMarkerY - 10.0f, 0.0f, 20.0f, 1.0f, 0xAA55C6B0);

    // Zoom controls.
    const float zoomButton = std::clamp(panelSize * 0.12f, 22.0f, 32.0f);
    const float zoomX = panelX + panelSize - zoomButton - 8.0f;
    const float zoomPlusY = mapBottom - zoomButton * 2.0f - 5.0f;
    const float zoomMinusY = mapBottom - zoomButton - 1.0f;
    rect(zoomX, zoomPlusY, zoomButton, zoomButton, 0xDD293541, 4.0f, true);
    rect(zoomX, zoomMinusY, zoomButton, zoomButton, 0xDD293541, 4.0f, true);
    text(zoomX + zoomButton * 0.31f, zoomPlusY + zoomButton * 0.13f,
         "+", 15.0f * textScale, 0xFFEAF0F5);
    text(zoomX + zoomButton * 0.34f, zoomMinusY + zoomButton * 0.12f,
         "-", 15.0f * textScale, 0xFFEAF0F5);

    const float footerY = panelY + panelHeight - footerH;
    rect(panelX + 1.0f, footerY, panelSize - 2.0f, footerH - 1.0f,
         0xE91A222C, 6.0f, true);

    if (!hasPosition) {
        text(panelX + 12.0f, footerY + 9.0f,
             "WAITING FOR PLAYER", 8.0f * textScale, 0xFFFFC56A);
    } else if (!hasSeed) {
        text(panelX + 12.0f, footerY + 9.0f,
             "SET WORLD SEED IN SETTINGS", 8.0f * textScale, 0xFFFFC56A);
    } else if (selectedValid) {
        auto selected = std::find_if(markers.begin(), markers.end(),
            [selectedX, selectedZ](const auto& marker) {
                return marker.x == selectedX && marker.z == selectedZ;
            });
        if (selected != markers.end()) {
            text(panelX + 11.0f, footerY + 5.0f,
                markerDescription(*selected), 8.0f * textScale, 0xFFEAF0F5);
            text(panelX + 11.0f, footerY + 19.0f,
                "X " + std::to_string(selected->x) + "  Z " +
                std::to_string(selected->z) + "  " +
                std::to_string(static_cast<int>(selected->distance)) + "m",
                7.0f * textScale, 0xFF9EABB9);
        } else {
            text(panelX + 11.0f, footerY + 9.0f,
                "NO STRUCTURE SELECTED", 8.0f * textScale, 0xFF9EABB9);
        }
    } else {
        text(panelX + 11.0f, footerY + 6.0f,
            std::to_string(markers.size()) + " STRUCTURE ATTEMPTS",
            8.0f * textScale, 0xFFEAF0F5);
        text(panelX + 11.0f, footerY + 19.0f,
            "CANDIDATES - BIOME NOT VERIFIED",
            6.6f * textScale, 0xFFFFC56A);
    }

    pl::modmenu::submitDrawCommands(moduleId, commands);
}

bool SeedMapModule::onTouch(const pl::input::TouchEvent& event) {
    if (!enabled) return false;
    std::lock_guard lock(m_stateMutex);
    if (!m_showMap) return false;

    const float x = event.x;
    const float y = event.y;
    const bool inside = x >= m_lastPanelX && x <= m_lastPanelX + m_lastPanelSize &&
        y >= m_lastPanelY && y <= m_lastPanelY + m_lastPanelHeight;
    const bool insideMap = x >= m_lastMapLeft && x <= m_lastMapLeft + m_lastMapWidth &&
        y >= m_lastMapTop && y <= m_lastMapTop + m_lastMapHeight;

    if (event.action == kActionDown) {
        // Never capture touches outside the actual map panel. In expanded mode
        // the old code swallowed every screen touch and immediately collapsed.
        if (!inside) return false;
        m_touchTracking = true;
        m_touchPointerId = event.pointerId;
        m_touchDownX = x;
        m_touchDownY = y;
        m_touchStartHudX = hudPosX;
        m_touchStartHudY = hudPosY;
        m_touchStartedOnMap = insideMap;
        m_touchMoved = false;
        return true;
    }

    if (!m_touchTracking || event.pointerId != m_touchPointerId) return false;

    if (event.action == kActionMove) {
        if (std::fabs(x - m_touchDownX) > 5.0f || std::fabs(y - m_touchDownY) > 5.0f) {
            m_touchMoved = true;
            if (m_touchStartedOnMap) {
                const float scale = std::min(m_lastMapWidth, m_lastMapHeight) /
                    (2.0f * std::max(1.0f, m_lastMapRadius));
                if (scale > 0.00001f) {
                    m_panOffsetX -= static_cast<int>(std::lround((x - m_touchDownX) / scale));
                    m_panOffsetZ -= static_cast<int>(std::lround((y - m_touchDownY) / scale));
                    m_touchDownX = x;
                    m_touchDownY = y;
                }
            } else if (!m_lastBigMap) {
                hudPosX = std::clamp(m_touchStartHudX + x - m_touchDownX,
                    4.0f, 4096.0f);
                hudPosY = std::clamp(m_touchStartHudY + y - m_touchDownY,
                    4.0f, 4096.0f);
            }
        }
        return true;
    }

    if (event.action == kActionCancel) {
        m_touchTracking = false;
        m_touchPointerId = -1;
        m_touchStartedOnMap = false;
        m_touchMoved = false;
        return true;
    }

    if (event.action != kActionUp) return true;

    const bool tap = !m_touchMoved;
    if (tap) {
        if (inside) {
            const float buttonSize = std::clamp(m_lastPanelSize * 0.13f, 24.0f, 34.0f);
            const float actionX = m_lastPanelX + m_lastPanelSize - buttonSize - 7.0f;
            const float actionY = m_lastPanelY + 6.0f;
            if (x >= actionX && x <= actionX + buttonSize &&
                y >= actionY && y <= actionY + buttonSize) {
                m_bigMap = !m_bigMap;
                m_markersDirty = true;
                m_hasSelectedMarker = false;
            } else {
                const float zoomButton = std::clamp(m_lastPanelSize * 0.12f, 22.0f, 32.0f);
                const float zoomX = m_lastPanelX + m_lastPanelSize - zoomButton - 8.0f;
                const float mapBottom = m_lastMapTop + m_lastMapHeight;
                const float plusY = mapBottom - zoomButton * 2.0f - 5.0f;
                const float minusY = mapBottom - zoomButton - 1.0f;
                if (x >= zoomX && x <= zoomX + zoomButton &&
                    y >= plusY && y <= plusY + zoomButton) {
                    m_mapRadiusBlocks = std::max(256, m_mapRadiusBlocks / 2);
                    m_markersDirty = true;
                } else if (x >= zoomX && x <= zoomX + zoomButton &&
                    y >= minusY && y <= minusY + zoomButton) {
                    m_mapRadiusBlocks = std::min(8192, m_mapRadiusBlocks * 2);
                    m_markersDirty = true;
                } else {
                    const float mapLeft = m_lastMapLeft;
                    const float mapTop = m_lastMapTop;
                    const float mapWidth = m_lastMapWidth;
                    const float mapHeight = m_lastMapHeight;
                    const float cx = mapLeft + mapWidth * 0.5f;
                    const float cy = mapTop + mapHeight * 0.5f;
                    const float viewCenterX = static_cast<float>(m_playerX + m_panOffsetX);
                    const float viewCenterZ = static_cast<float>(m_playerZ + m_panOffsetZ);
                    const float scale = std::min(mapWidth, mapHeight) /
                        (2.0f * std::max(1.0f, m_lastMapRadius));
                    float bestDistanceSquared = 24.0f * 24.0f;
                    const bedaim::seedmap::StructureMarker* best = nullptr;
                    for (const auto& marker : m_markers) {
                        const float px = cx + (marker.x - viewCenterX) * scale;
                        const float py = cy + (marker.z - viewCenterZ) * scale;
                        const float mx = px - x;
                        const float my = py - y;
                        const float d2 = mx * mx + my * my;
                        if (d2 < bestDistanceSquared) {
                            bestDistanceSquared = d2;
                            best = &marker;
                        }
                    }
                    if (best) {
                        m_hasSelectedMarker = true;
                        m_selectedMarkerX = best->x;
                        m_selectedMarkerZ = best->z;
                    } else {
                        m_hasSelectedMarker = false;
                    }
                }
            }
        }
    }

    m_touchTracking = false;
    m_touchPointerId = -1;
    m_touchStartedOnMap = false;
    m_touchMoved = false;
    return true;
}

std::string SeedMapModule::readClipboardText() {
    JavaVM* vm = bedrocktools::core::javaVm();
    if (!vm) return {};

    JNIEnv* env = nullptr;
    bool attachedHere = false;
    const jint getEnvResult = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvResult == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return {};
        attachedHere = true;
    } else if (getEnvResult != JNI_OK || !env) {
        return {};
    }

    std::string result;
    jclass activityThread = nullptr;
    jclass contextClass = nullptr;
    jclass clipboardClass = nullptr;
    jclass clipDataClass = nullptr;
    jclass itemClass = nullptr;
    jclass charSequenceClass = nullptr;
    jobject application = nullptr;
    jobject clipboardManager = nullptr;
    jobject clipData = nullptr;
    jobject item = nullptr;
    jobject charSequence = nullptr;
    jstring serviceName = nullptr;
    jstring textString = nullptr;

    activityThread = env->FindClass("android/app/ActivityThread");
    if (activityThread && !env->ExceptionCheck()) {
        const jmethodID currentApplication = env->GetStaticMethodID(activityThread,
            "currentApplication", "()Landroid/app/Application;");
        if (currentApplication && !env->ExceptionCheck())
            application = env->CallStaticObjectMethod(activityThread, currentApplication);
    }

    if (application && !env->ExceptionCheck()) {
        contextClass = env->FindClass("android/content/Context");
        if (contextClass && !env->ExceptionCheck()) {
            const jmethodID getSystemService = env->GetMethodID(contextClass,
                "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
            serviceName = env->NewStringUTF("clipboard");
            if (getSystemService && serviceName && !env->ExceptionCheck())
                clipboardManager = env->CallObjectMethod(application, getSystemService, serviceName);
        }
    }

    if (clipboardManager && !env->ExceptionCheck()) {
        clipboardClass = env->FindClass("android/content/ClipboardManager");
        if (clipboardClass && !env->ExceptionCheck()) {
            const jmethodID getPrimaryClip = env->GetMethodID(clipboardClass,
                "getPrimaryClip", "()Landroid/content/ClipData;");
            if (getPrimaryClip && !env->ExceptionCheck())
                clipData = env->CallObjectMethod(clipboardManager, getPrimaryClip);
        }
    }

    if (clipData && !env->ExceptionCheck()) {
        clipDataClass = env->FindClass("android/content/ClipData");
        if (clipDataClass && !env->ExceptionCheck()) {
            const jmethodID getItemCount = env->GetMethodID(clipDataClass, "getItemCount", "()I");
            const jmethodID getItemAt = env->GetMethodID(clipDataClass, "getItemAt",
                "(I)Landroid/content/ClipData$Item;");
            if (getItemCount && getItemAt && !env->ExceptionCheck()) {
                const jint count = env->CallIntMethod(clipData, getItemCount);
                if (count > 0 && !env->ExceptionCheck())
                    item = env->CallObjectMethod(clipData, getItemAt, static_cast<jint>(0));
            }
        }
    }

    if (item && !env->ExceptionCheck()) {
        itemClass = env->FindClass("android/content/ClipData$Item");
        if (itemClass && !env->ExceptionCheck()) {
            const jmethodID coerceToText = env->GetMethodID(itemClass, "coerceToText",
                "(Landroid/content/Context;)Ljava/lang/CharSequence;");
            if (coerceToText && !env->ExceptionCheck())
                charSequence = env->CallObjectMethod(item, coerceToText, application);
        }
    }

    if (charSequence && !env->ExceptionCheck()) {
        charSequenceClass = env->GetObjectClass(charSequence);
        if (charSequenceClass && !env->ExceptionCheck()) {
            const jmethodID toString = env->GetMethodID(charSequenceClass, "toString", "()Ljava/lang/String;");
            if (toString && !env->ExceptionCheck())
                textString = static_cast<jstring>(env->CallObjectMethod(charSequence, toString));
        }
    }

    if (textString && !env->ExceptionCheck()) {
        const char* utf = env->GetStringUTFChars(textString, nullptr);
        if (utf) {
            result.assign(utf);
            env->ReleaseStringUTFChars(textString, utf);
        }
    }

    clearJniException(env);
    if (textString) env->DeleteLocalRef(textString);
    if (charSequenceClass) env->DeleteLocalRef(charSequenceClass);
    if (charSequence) env->DeleteLocalRef(charSequence);
    if (itemClass) env->DeleteLocalRef(itemClass);
    if (item) env->DeleteLocalRef(item);
    if (clipDataClass) env->DeleteLocalRef(clipDataClass);
    if (clipData) env->DeleteLocalRef(clipData);
    if (clipboardClass) env->DeleteLocalRef(clipboardClass);
    if (clipboardManager) env->DeleteLocalRef(clipboardManager);
    if (serviceName) env->DeleteLocalRef(serviceName);
    if (contextClass) env->DeleteLocalRef(contextClass);
    if (application) env->DeleteLocalRef(application);
    if (activityThread) env->DeleteLocalRef(activityThread);
    if (attachedHere) vm->DetachCurrentThread();
    return result;
}

bool SeedMapModule::pasteClipboardSeed() {
    const std::string clipboard = readClipboardText();
    std::string detected = parseSeedAnnouncement(clipboard);
    if (detected.empty()) {
        const std::string candidate = trimCopy(clipboard);
        if (isValidSeedNumber(candidate)) detected = candidate;
    }
    if (detected.empty()) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning,
            "SeedMap", "clipboard_seed=unavailable; no numeric world seed found");
        return false;
    }

    {
        std::lock_guard lock(m_stateMutex);
        m_seed = detected;
        m_markersDirty = true;
        m_hasSelectedMarker = false;
    }
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,
        "SeedMap", "clipboard_seed=success; numeric world seed loaded");
    return true;
}

void SeedMapModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("seed")
        .category("seed", "Seed Map", "The in-game map uses the world seed to calculate Bedrock structure candidates around your live position.")
        .category("appearance", "Map appearance", "Configure the minimap overlay and its placement.");

    ConfigNodeV2 seed;
    seed.id = "seed";
    seed.key = "seed";
    seed.category = "seed";
    seed.title = "World seed";
    seed.description = "Enter the numeric 64-bit Bedrock world seed. The map cannot reveal a seed hidden by a multiplayer server.";
    seed.type = ConfigControlTypeV2::Text;
    seed.defaultValue = "";
    {
        std::lock_guard lock(m_stateMutex);
        seed.currentValue = m_seed;
    }
    seed.hasCurrentValue = true;
    seed.placeholder = "Enter numeric world seed";
    seed.maxLength = 20;
    schema.node(std::move(seed));

    ConfigNodeV2 paste;
    paste.id = "pasteClipboardSeed";
    paste.key = "pasteClipboardSeed";
    paste.category = "seed";
    paste.title = "Import seed from clipboard";
    paste.description = "Paste a numeric seed copied from world settings or Déesse UI's Seed World Copy option.";
    paste.type = ConfigControlTypeV2::Button;
    paste.actionValue = "paste";
    schema.node(std::move(paste));

    ConfigNodeV2 info;
    info.id = "seedMapInfo";
    info.key = "seedMapInfo";
    info.category = "seed";
    info.title = "Structure prediction";
    info.description = "The first engine pass plots Bedrock generation-attempt sites. Biome and terrain confirmation is not implemented yet, so markers are candidates rather than guaranteed generated structures. Strongholds, trial chambers, geodes, mineshafts and other not-yet-supported structure types are not plotted.";
    info.type = ConfigControlTypeV2::Info;
    schema.node(std::move(info));

    auto addToggle = [&](const char* id, const char* title, const char* description,
                         const char* key, bool current) {
        ConfigNodeV2 node;
        node.id = id; node.key = key; node.category = "appearance";
        node.title = title; node.description = description;
        node.type = ConfigControlTypeV2::Toggle;
        node.defaultValue = current ? "true" : "false";
        node.currentValue = node.defaultValue; node.hasCurrentValue = true;
        schema.node(std::move(node));
    };
    auto addSliderInt = [&](const char* id, const char* title, const char* description,
                            const char* key, int current, int minimum, int maximum,
                            int step, const char* unit) {
        ConfigNodeV2 node;
        node.id = id; node.key = key; node.category = "appearance";
        node.title = title; node.description = description;
        node.type = ConfigControlTypeV2::SliderInt;
        node.defaultValue = std::to_string(current);
        node.currentValue = node.defaultValue; node.hasCurrentValue = true;
        node.minValue = std::to_string(minimum);
        node.maxValue = std::to_string(maximum);
        node.step = std::to_string(step); node.unit = unit;
        schema.node(std::move(node));
    };
    auto addSliderFloat = [&](const char* id, const char* title, const char* description,
                              const char* key, float current, float minimum,
                              float maximum, float step, const char* unit) {
        ConfigNodeV2 node;
        node.id = id; node.key = key; node.category = "appearance";
        node.title = title; node.description = description;
        node.type = ConfigControlTypeV2::SliderFloat;
        node.defaultValue = std::to_string(current);
        node.currentValue = node.defaultValue; node.hasCurrentValue = true;
        node.minValue = std::to_string(minimum);
        node.maxValue = std::to_string(maximum);
        node.step = std::to_string(step); node.unit = unit;
        schema.node(std::move(node));
    };
    {
        std::lock_guard lock(m_stateMutex);
        addToggle("showMap", "Show map", "Display the in-game Seed Map overlay.",
                  "m_showMap", m_showMap);
        addToggle("showStructures", "Show structures", "Show or hide structure candidates.",
                  "m_showStructures", m_showStructures);
        addToggle("showGrid", "Grid lines", "Display chunk grid lines over the map.",
                  "m_showChunkGrid", m_showChunkGrid);
        addToggle("showCoordinates", "Coordinates", "Display live player and selected marker coordinates.",
                  "m_showCoordinates", m_showCoordinates);
        addSliderInt("mapRadius", "Map radius", "World blocks visible from the map center.",
                     "m_mapRadiusBlocks", m_mapRadiusBlocks, 256, 8192, 256, "blocks");
        addSliderFloat("mapSize", "Map size", "Size of the compact in-game map.",
                       "m_mapSize", m_mapSize, 160.0f, 420.0f, 10.0f, "px");
    }

    setConfigSchemaJson(moduleId, schema.toJson());
}

bool SeedMapModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "pasteClipboardSeed") {
        pasteClipboardSeed();
        return true;
    }
    if (key == "seed") {
        const std::string seed = trimCopy(value);
        if (!seed.empty() && !isValidSeedNumber(seed)) return false;
        std::lock_guard lock(m_stateMutex);
        if (m_seed != seed) {
            m_seed = seed;
            m_markersDirty = true;
            m_hasSelectedMarker = false;
        }
        return true;
    }

    std::lock_guard lock(m_stateMutex);
    if (key == "m_showMap") return parseBoolValue(value, m_showMap);
    if (key == "m_showStructures") return parseBoolValue(value, m_showStructures);
    if (key == "m_showChunkGrid") return parseBoolValue(value, m_showChunkGrid);
    if (key == "m_showCoordinates") return parseBoolValue(value, m_showCoordinates);
    if (key == "m_mapRadiusBlocks") {
        int parsed = 0;
        if (!parseIntValue(value, parsed)) return false;
        m_mapRadiusBlocks = std::clamp(parsed, 256, 8192);
        m_markersDirty = true;
        return true;
    }
    if (key == "m_mapSize") {
        float parsed = 0.0f;
        if (!parseFloatValue(value, parsed)) return false;
        m_mapSize = std::clamp(parsed, 160.0f, 420.0f);
        return true;
    }
    if (key == "hudPosX") {
        float parsed = 0.0f;
        if (!parseFloatValue(value, parsed)) return false;
        hudPosX = std::max(4.0f, parsed);
        return true;
    }
    if (key == "hudPosY") {
        float parsed = 0.0f;
        if (!parseFloatValue(value, parsed)) return false;
        hudPosY = std::max(4.0f, parsed);
        return true;
    }
    return false;
}

void SeedMapModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    {
        std::lock_guard lock(m_stateMutex);
        m_seed = json.value("seed", m_seed);
        m_showMap = json.value("m_showMap", m_showMap);
        m_showStructures = json.value("m_showStructures", m_showStructures);
        m_showChunkGrid = json.value("m_showChunkGrid", m_showChunkGrid);
        m_showCoordinates = json.value("m_showCoordinates", m_showCoordinates);
        m_mapRadiusBlocks = std::clamp(json.value("m_mapRadiusBlocks", m_mapRadiusBlocks), 256, 8192);
        m_mapSize = std::clamp(json.value("m_mapSize", m_mapSize), 160.0f, 420.0f);
        m_panOffsetX = json.value("m_panOffsetX", m_panOffsetX);
        m_panOffsetZ = json.value("m_panOffsetZ", m_panOffsetZ);
        hudPosX = std::max(4.0f, json.value("hudPosX", hudPosX));
        hudPosY = std::max(4.0f, json.value("hudPosY", hudPosY));
        m_markersDirty = true;
    }
    // The menu schema may have been registered before persisted settings were
    // loaded. Re-publish it so every control displays the restored value.
    onMenuRegistered();
}

void SeedMapModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard lock(m_stateMutex);
    json["seed"] = m_seed;
    json["m_showMap"] = m_showMap;
    json["m_showStructures"] = m_showStructures;
    json["m_showChunkGrid"] = m_showChunkGrid;
    json["m_showCoordinates"] = m_showCoordinates;
    json["m_mapRadiusBlocks"] = m_mapRadiusBlocks;
    json["m_panOffsetX"] = m_panOffsetX;
    json["m_panOffsetZ"] = m_panOffsetZ;
    json["m_mapSize"] = m_mapSize;
    json["hudPosX"] = hudPosX;
    json["hudPosY"] = hudPosY;
}
