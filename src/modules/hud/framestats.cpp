#include "framestats.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <pl/ModMenu.hpp>

namespace {
std::string fixedOneDecimal(float value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.1f", static_cast<double>(value));
    return buffer;
}
}

FrameStatsModule::FrameStatsModule()
    : Module("Frame Stats", "Shows live FPS, frame time, average FPS and 1% low over a rolling window.") {}

void FrameStatsModule::onEnable() {
    std::lock_guard lock(m_mutex);
    m_lastFrameAt = {};
    m_window.reset();
}

void FrameStatsModule::onDisable() {
    std::lock_guard lock(m_mutex);
    m_lastFrameAt = {};
    m_window.reset();
}

void FrameStatsModule::onFrame() {
    if (!enabled) return;
    const auto now = std::chrono::steady_clock::now();
    const auto timestampMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());

    float x = 0.0f, y = 0.0f, size = 18.0f, opacity = 0.55f;
    bool background = true, showAverage = true, showOnePercentLow = true;
    bedaim::hud::FrameStatsSnapshot stats;
    {
        std::lock_guard lock(m_mutex);
        x = hudPosX; y = hudPosY; size = m_size; opacity = m_backgroundOpacity;
        background = m_background; showAverage = m_showAverage;
        showOnePercentLow = m_showOnePercentLow;
        m_window.setWindowMs(static_cast<std::uint64_t>(m_windowSeconds * 1000.0f));
        if (m_lastFrameAt.time_since_epoch().count() != 0) {
            const float frameMs = std::chrono::duration<float, std::milli>(now - m_lastFrameAt).count();
            m_window.addSample(timestampMs, frameMs);
        }
        m_lastFrameAt = now;
        stats = m_window.snapshot();
    }

    std::string text;
    if (stats.sampleCount == 0) {
        text = "FPS -- | Frame --";
    } else {
        text = "FPS " + std::to_string(static_cast<int>(std::lround(stats.currentFps))) +
               " | " + fixedOneDecimal(stats.averageFrameMs) + " ms";
        if (showAverage) text += " | Avg " + std::to_string(static_cast<int>(std::lround(stats.averageFps)));
        if (showOnePercentLow) text += " | 1% " + std::to_string(static_cast<int>(std::lround(stats.onePercentLowFps)));
    }

    const float width = static_cast<float>(text.size()) * size * 0.58f + 12.0f;
    const float height = size + 8.0f;
    std::vector<pl::modmenu::DrawCommand> commands;
    if (background) {
        pl::modmenu::DrawCommand box{};
        box.type = pl::modmenu::DrawCommandType::RectFilled;
        box.x = x; box.y = y; box.w = width; box.h = height;
        const auto alpha = static_cast<std::uint32_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f);
        box.color = alpha << 24U;
        commands.push_back(std::move(box));
    }
    pl::modmenu::DrawCommand label{};
    label.type = pl::modmenu::DrawCommandType::Text;
    label.x = x; label.y = y; label.w = width; label.h = height;
    label.size = size; label.color = 0xFFFFFFFF; label.text = text;
    commands.push_back(std::move(label));
    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void FrameStatsModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("performance")
        .category("performance", "Performance", "Live frame pacing statistics")
        .category("appearance", "Appearance", "Overlay size and background");

    auto toggle = [&](const char* key, const char* title, const char* category, const char* description) {
        ConfigNodeV2 node;
        node.id = key; node.key = key; node.title = title; node.category = category;
        node.description = description; node.type = ConfigControlTypeV2::Toggle;
        schema.node(std::move(node));
    };
    auto slider = [&](const char* key, const char* title, const char* category,
                      const char* minValue, const char* maxValue, const char* step,
                      const char* unit, const char* description) {
        ConfigNodeV2 node;
        node.id = key; node.key = key; node.title = title; node.category = category;
        node.description = description; node.type = ConfigControlTypeV2::SliderFloat;
        node.minValue = minValue; node.maxValue = maxValue; node.step = step; node.unit = unit;
        schema.node(std::move(node));
    };

    toggle("m_showAverage", "Show average FPS", "performance", "Display average FPS over the rolling window.");
    toggle("m_showOnePercentLow", "Show 1% low", "performance", "Display FPS derived from the slowest 1% of frame times.");
    slider("m_windowSeconds", "Sample window", "performance", "2", "10", "1", " s", "Seconds of recent frame timings to retain.");
    toggle("m_background", "Background", "appearance", "Draw a translucent background behind the text.");
    slider("m_size", "Text size", "appearance", "8", "48", "1", " px", "Text size for the overlay.");
    slider("m_backgroundOpacity", "Background opacity", "appearance", "0", "1", "0.05", "", "Opacity of the background.");
    ConfigNodeV2 keybindNode;
    keybindNode.id = "keybind"; keybindNode.key = "keybind"; keybindNode.title = "Keybind";
    keybindNode.category = "performance"; keybindNode.type = ConfigControlTypeV2::Keybind;
    schema.node(std::move(keybindNode));
    setConfigSchemaJson(moduleId, schema.toJson());
}

void FrameStatsModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard lock(m_mutex);
    if (json.contains("hudPosX") && json["hudPosX"].is_number()) hudPosX = std::clamp(json["hudPosX"].get<float>(), 0.0f, 4000.0f);
    if (json.contains("hudPosY") && json["hudPosY"].is_number()) hudPosY = std::clamp(json["hudPosY"].get<float>(), 0.0f, 4000.0f);
    if (json.contains("m_size") && json["m_size"].is_number()) m_size = std::clamp(json["m_size"].get<float>(), 8.0f, 48.0f);
    if (json.contains("m_backgroundOpacity") && json["m_backgroundOpacity"].is_number()) m_backgroundOpacity = std::clamp(json["m_backgroundOpacity"].get<float>(), 0.0f, 1.0f);
    if (json.contains("m_windowSeconds") && json["m_windowSeconds"].is_number()) m_windowSeconds = std::clamp(json["m_windowSeconds"].get<float>(), 2.0f, 10.0f);
    if (json.contains("m_background") && json["m_background"].is_boolean()) m_background = json["m_background"].get<bool>();
    if (json.contains("m_showAverage") && json["m_showAverage"].is_boolean()) m_showAverage = json["m_showAverage"].get<bool>();
    if (json.contains("m_showOnePercentLow") && json["m_showOnePercentLow"].is_boolean()) m_showOnePercentLow = json["m_showOnePercentLow"].get<bool>();
}

void FrameStatsModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard lock(m_mutex);
    json["hudPosX"] = hudPosX; json["hudPosY"] = hudPosY; json["isHudModule"] = true;
    json["m_size"] = m_size; json["m_backgroundOpacity"] = m_backgroundOpacity;
    json["m_windowSeconds"] = m_windowSeconds; json["m_background"] = m_background;
    json["m_showAverage"] = m_showAverage; json["m_showOnePercentLow"] = m_showOnePercentLow;
}
