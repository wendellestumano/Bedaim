#include "breakindicator.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/GameModeActionEvent.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>


namespace {
struct ReadableRegion {
    std::uintptr_t begin{};
    std::uintptr_t end{};
};

// Break progress is read from a version-sensitive native GameMode layout.
// Check the current process mappings before copying any bytes; fail closed if
// procfs is unavailable or the field crosses a mapping boundary.
bool isReadableRange(const void* pointer, std::size_t size) {
    if (!pointer || size == 0) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    if (size > std::numeric_limits<std::uintptr_t>::max() - begin) return false;
    const auto end = begin + size;

    thread_local std::vector<ReadableRegion> readableRegions;
    thread_local std::chrono::steady_clock::time_point mapsRefreshed{};
    thread_local bool mapsCacheValid = false;
    const auto now = std::chrono::steady_clock::now();

    if (!mapsCacheValid || now < mapsRefreshed ||
        now - mapsRefreshed > std::chrono::milliseconds(50)) {
        std::vector<ReadableRegion> refreshed;
        std::FILE* maps = std::fopen("/proc/self/maps", "r");
        if (!maps) {
            readableRegions.clear();
            mapsCacheValid = false;
            return false;
        }
        char line[512]{};
        while (std::fgets(line, sizeof(line), maps)) {
            unsigned long long regionBegin = 0, regionEnd = 0;
            char permissions[5]{};
            if (std::sscanf(line, "%llx-%llx %4s", &regionBegin, &regionEnd,
                            permissions) == 3 &&
                permissions[0] == 'r' && regionEnd > regionBegin) {
                refreshed.push_back({static_cast<std::uintptr_t>(regionBegin),
                                     static_cast<std::uintptr_t>(regionEnd)});
            }
        }
        std::fclose(maps);
        if (refreshed.empty()) {
            readableRegions.clear();
            mapsCacheValid = false;
            return false;
        }
        readableRegions.swap(refreshed);
        mapsRefreshed = now;
        mapsCacheValid = true;
    }

    const auto it = std::upper_bound(
        readableRegions.begin(), readableRegions.end(), begin,
        [](std::uintptr_t address, const ReadableRegion& region) {
            return address < region.begin;
        });
    if (it == readableRegions.begin()) return false;
    const auto& region = *std::prev(it);
    return begin >= region.begin && end <= region.end;
}
} // namespace

static void indicatorHSVtoRGB(float h, float s, float v, float& out_r, float& out_g, float& out_b) {
    if (s == 0.0f) { out_r = out_g = out_b = v; return; }
    h = std::fmod(h, 1.0f) * 6.0f;
    int i = (int)std::floor(h);
    float f = h - (float)i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    switch (i) {
        case 0: out_r = v; out_g = t; out_b = p; break;
        case 1: out_r = q; out_g = v; out_b = p; break;
        case 2: out_r = p; out_g = v; out_b = t; break;
        case 3: out_r = p; out_g = q; out_b = v; break;
        case 4: out_r = t; out_g = p; out_b = v; break;
        default: out_r = v; out_g = p; out_b = q; break;
    }
}

BreakIndicatorModule::BreakIndicatorModule()
    : Module("Break Indicator", "Displays a progress bar when breaking blocks.") {
}

void BreakIndicatorModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::GameModeActionEvent>([this](auto& event) {
        // EventBus subscriptions survive module disablement, so guard inside
        // the callback rather than relying on ModuleRegistry::onFrame().
        if (!enabled) {
            m_gameMode = nullptr;
            m_breaking = false;
            m_progress = 0.0f;
            return;
        }

        if (event.action == bedrocktools::events::GameModeAction::StartDestroyBlock) {
            m_gameMode = event.gameMode;
            m_breaking = m_gameMode != nullptr;
            m_progress = 0.0f;
            m_lastUpdate = std::chrono::steady_clock::now();
        } else if (event.action == bedrocktools::events::GameModeAction::StopDestroyBlock &&
                   (!m_gameMode || m_gameMode == event.gameMode)) {
            m_gameMode = nullptr;
            m_breaking = false;
            m_progress = 0.0f;
            m_lastUpdate = std::chrono::steady_clock::now();
        }
    });

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([this](auto&) {
        if (!enabled || !m_breaking || !m_gameMode) return;

        const auto gameModeAddress = reinterpret_cast<std::uintptr_t>(m_gameMode);
        constexpr auto progressOffset =
            bedrocktools::sdk::offsets::GameMode::mDestroyProgress;
        if (!isReadableRange(m_gameMode, sizeof(void*)) ||
            gameModeAddress > std::numeric_limits<std::uintptr_t>::max() - progressOffset) {
            m_progress = 0.0f;
            return;
        }

        const auto* progressAddress =
            reinterpret_cast<const void*>(gameModeAddress + progressOffset);
        float progress = 0.0f;
        if (!isReadableRange(progressAddress, sizeof(progress))) {
            // Offsets can drift between Bedrock builds. Do not dereference an
            // unreadable field; the HUD is optional and must never crash the game.
            m_progress = 0.0f;
            m_lastUpdate = std::chrono::steady_clock::now();
            return;
        }
        std::memcpy(&progress, progressAddress, sizeof(progress));

        if (std::isfinite(progress) && progress >= 0.0f && progress <= 1.05f) {
            m_progress = std::clamp(progress, 0.0f, 1.0f);
        } else {
            m_progress = 0.0f;
        }
        m_lastUpdate = std::chrono::steady_clock::now();
    });
}

void BreakIndicatorModule::onEnable() {
    m_gameMode = nullptr;
    m_breaking = false;
    m_progress = 0.0f;
    m_lastUpdate = std::chrono::steady_clock::now();
}

void BreakIndicatorModule::onDisable() {
    m_gameMode = nullptr;
    m_breaking = false;
    m_progress = 0.0f;
}

void BreakIndicatorModule::onFrame() {
    if (!enabled) return;

    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastUpdate).count() > 250) {
        m_gameMode = nullptr;
        m_breaking = false;
        m_progress = 0.0f;
    }

    float drawProgress = m_breaking ? m_progress : 0.0f;

    if (drawProgress <= 0.0f && !m_alwaysShow) {
        std::vector<PLModMenu_DrawCommand> cmds;
        submitDrawCommands(moduleId, cmds);
        return;
    }

    std::vector<PLModMenu_DrawCommand> cmds;

    if (m_outline) {
        PLModMenu_DrawCommand outlineCmd = {};
        outlineCmd.type = PL_DRAW_RECT_FILLED;
        outlineCmd.x = hudPosX - m_outlineThickness;
        outlineCmd.y = hudPosY - m_outlineThickness;
        outlineCmd.w = m_width + (m_outlineThickness * 2.0f);
        outlineCmd.h = m_height + (m_outlineThickness * 2.0f);
        outlineCmd.x3 = m_cornerRadius + m_outlineThickness;
        outlineCmd.color = (0xFF << 24) | (m_outlineColorHex & 0x00FFFFFF);
        cmds.push_back(outlineCmd);
    }

    if (m_background) {
        PLModMenu_DrawCommand bgCmd = {};
        bgCmd.type = PL_DRAW_RECT_FILLED;
        bgCmd.x = hudPosX;
        bgCmd.y = hudPosY;
        bgCmd.w = m_width;
        bgCmd.h = m_height;
        bgCmd.x3 = m_cornerRadius;
        bgCmd.color = ((int)(m_backgroundOpacity * 255.0f) << 24) | 0x000000;
        cmds.push_back(bgCmd);
    }

    uint32_t barColor = m_barColorHex;
    if (m_rainbow) {
        m_rainbowHue += 0.002f * m_rainbowSpeed;
        if (m_rainbowHue > 1.0f) m_rainbowHue -= 1.0f;
        float r, g, b;
        indicatorHSVtoRGB(m_rainbowHue, 1.0f, 1.0f, r, g, b);
        barColor = (0xFF << 24) | (((int)(r * 255)) << 16) | (((int)(g * 255)) << 8) | ((int)(b * 255));
    } else {
        barColor = (0xFF << 24) | (barColor & 0x00FFFFFF);
    }

    if (drawProgress > 0.001f) {
        PLModMenu_DrawCommand barCmd = {};
        barCmd.type = PL_DRAW_RECT_FILLED;
        barCmd.x = hudPosX;
        barCmd.y = hudPosY;
        barCmd.w = m_width * drawProgress;
        barCmd.h = m_height;
        barCmd.x3 = m_cornerRadius;
        barCmd.color = barColor;
        cmds.push_back(barCmd);
    }

    std::string text = std::to_string((int)(drawProgress * 100)) + "%";
    PLModMenu_DrawCommand txtCmd = {};
    txtCmd.type = PL_DRAW_TEXT;
    txtCmd.x = hudPosX;
    txtCmd.y = hudPosY;
    txtCmd.w = m_width;
    txtCmd.h = m_height;
    txtCmd.color = (0xFF << 24) | (m_textColorHex & 0x00FFFFFF);
    txtCmd.size = m_textSize;
    txtCmd.text = text.c_str();
    cmds.push_back(txtCmd);

    submitDrawCommands(moduleId, cmds);
}

void BreakIndicatorModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    
    if (j.contains("m_width")) m_width = j["m_width"].get<float>();
    if (j.contains("m_height")) m_height = j["m_height"].get<float>();
    if (j.contains("m_textSize")) m_textSize = j["m_textSize"].get<float>();
    if (j.contains("m_cornerRadius")) m_cornerRadius = j["m_cornerRadius"].get<float>();
    
    if (j.contains("m_background")) m_background = j["m_background"].get<bool>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    
    if (j.contains("barColorHex")) {
        std::string hexStr = j["barColorHex"].get<std::string>();
        if (hexStr.length() > 0 && hexStr[0] == '#') {
            try { m_barColorHex = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
        }
    }
    if (j.contains("m_rainbow")) m_rainbow = j["m_rainbow"].get<bool>();
    if (j.contains("m_rainbowSpeed")) m_rainbowSpeed = j["m_rainbowSpeed"].get<float>();
    
    if (j.contains("textColorHex")) {
        std::string hexStr = j["textColorHex"].get<std::string>();
        if (hexStr.length() > 0 && hexStr[0] == '#') {
            try { m_textColorHex = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
        }
    }
    
    if (j.contains("m_outline")) m_outline = j["m_outline"].get<bool>();
    if (j.contains("outlineColorHex")) {
        std::string hexStr = j["outlineColorHex"].get<std::string>();
        if (hexStr.length() > 0 && hexStr[0] == '#') {
            try { m_outlineColorHex = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
        }
    }
    if (j.contains("m_outlineThickness")) m_outlineThickness = j["m_outlineThickness"].get<float>();

    if (j.contains("m_alwaysShow")) m_alwaysShow = j["m_alwaysShow"].get<bool>();
}

void BreakIndicatorModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    
    j["m_width"] = m_width;
    j["m_height"] = m_height;
    j["m_textSize"] = m_textSize;
    j["m_cornerRadius"] = m_cornerRadius;
    
    j["m_background"] = m_background;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    
    char hexStr[10];
    snprintf(hexStr, sizeof(hexStr), "#%08X", m_barColorHex);
    j["barColorHex"] = std::string(hexStr);
    
    j["m_rainbow"] = m_rainbow;
    j["m_rainbowSpeed"] = m_rainbowSpeed;
    
    snprintf(hexStr, sizeof(hexStr), "#%08X", m_textColorHex);
    j["textColorHex"] = std::string(hexStr);
    
    j["m_outline"] = m_outline;
    
    snprintf(hexStr, sizeof(hexStr), "#%08X", m_outlineColorHex);
    j["outlineColorHex"] = std::string(hexStr);
    
    j["m_outlineThickness"] = m_outlineThickness;

    j["m_alwaysShow"] = m_alwaysShow;
}
