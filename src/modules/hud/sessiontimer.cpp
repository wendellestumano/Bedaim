#include "sessiontimer.hpp"
#include "SessionTimerFormat.hpp"
#include "modules/ModuleRegistry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <pl/ModMenu.hpp>

SessionTimerModule::SessionTimerModule()
    : Module("Session Timer", "Displays elapsed time since the module was enabled.") {}

void SessionTimerModule::onEnable() {
    std::lock_guard lock(m_configMutex);
    m_startedAt = std::chrono::steady_clock::now();
}

void SessionTimerModule::onFrame() {
    if (!enabled) return;

    float x, y, size, opacity;
    bool background, showHours;
    std::chrono::steady_clock::time_point startedAt;
    {
        std::lock_guard lock(m_configMutex);
        x = hudPosX;
        y = hudPosY;
        size = m_size;
        opacity = m_backgroundOpacity;
        background = m_background;
        showHours = m_showHours;
        startedAt = m_startedAt;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = startedAt.time_since_epoch().count() == 0
        ? 0LL
        : std::chrono::duration_cast<std::chrono::seconds>(now - startedAt).count();
    const std::string text = "Session: " + bedaim::hud::formatSessionDuration(elapsed, showHours);
    const float width = static_cast<float>(text.size()) * size * 0.58f + 12.0f;
    const float height = size + 8.0f;

    std::vector<pl::modmenu::DrawCommand> commands;
    if (background) {
        pl::modmenu::DrawCommand box{};
        box.type = pl::modmenu::DrawCommandType::RectFilled;
        box.x = x;
        box.y = y;
        box.w = width;
        box.h = height;
        const auto alpha = static_cast<std::uint32_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f);
        box.color = alpha << 24U;
        commands.push_back(std::move(box));
    }

    pl::modmenu::DrawCommand label{};
    label.type = pl::modmenu::DrawCommandType::Text;
    label.x = x;
    label.y = y;
    label.w = width;
    label.h = height;
    label.size = size;
    label.color = 0xFFFFFFFF;
    label.text = text;
    commands.push_back(std::move(label));
    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void SessionTimerModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("session")
        .category("session", "Session", "Elapsed session time display")
        .category("appearance", "Appearance", "Position and display style");

    auto add = [&](const char* key, const char* title, const char* category,
                   ConfigControlTypeV2 type) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.title = title;
        node.category = category;
        node.type = type;
        schema.node(std::move(node));
    };

    auto format = [&] {
        ConfigNodeV2 node;
        node.id = "m_showHours";
        node.key = "m_showHours";
        node.title = "Always show hours";
        node.category = "session";
        node.type = ConfigControlTypeV2::Toggle;
        schema.node(std::move(node));
    };
    format();
    add("m_background", "Background", "appearance", ConfigControlTypeV2::Toggle);

    ConfigNodeV2 size;
    size.id = "m_size";
    size.key = "m_size";
    size.title = "Text size";
    size.category = "appearance";
    size.type = ConfigControlTypeV2::SliderFloat;
    size.minValue = "6";
    size.maxValue = "100";
    size.step = "1";
    size.unit = " px";
    schema.node(std::move(size));

    ConfigNodeV2 opacity;
    opacity.id = "m_backgroundOpacity";
    opacity.key = "m_backgroundOpacity";
    opacity.title = "Background opacity";
    opacity.category = "appearance";
    opacity.type = ConfigControlTypeV2::SliderFloat;
    opacity.minValue = "0";
    opacity.maxValue = "1";
    opacity.step = "0.01";
    opacity.visibleWhen = {{"m_background", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(opacity));
    schema.node([] {
        ConfigNodeV2 node;
        node.id = "keybind";
        node.key = "keybind";
        node.title = "Keybind";
        node.category = "session";
        node.type = ConfigControlTypeV2::Keybind;
        return node;
    }());
    setConfigSchemaJson(moduleId, schema.toJson());
}

void SessionTimerModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard lock(m_configMutex);
    if (json.contains("hudPosX") && json["hudPosX"].is_number())
        hudPosX = std::clamp(json["hudPosX"].get<float>(), 0.0f, 4000.0f);
    if (json.contains("hudPosY") && json["hudPosY"].is_number())
        hudPosY = std::clamp(json["hudPosY"].get<float>(), 0.0f, 4000.0f);
    if (json.contains("m_size") && json["m_size"].is_number())
        m_size = std::clamp(json["m_size"].get<float>(), 6.0f, 100.0f);
    if (json.contains("m_backgroundOpacity") && json["m_backgroundOpacity"].is_number())
        m_backgroundOpacity = std::clamp(json["m_backgroundOpacity"].get<float>(), 0.0f, 1.0f);
    if (json.contains("m_background") && json["m_background"].is_boolean())
        m_background = json["m_background"].get<bool>();
    if (json.contains("m_showHours") && json["m_showHours"].is_boolean())
        m_showHours = json["m_showHours"].get<bool>();
}

void SessionTimerModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard lock(m_configMutex);
    json["hudPosX"] = hudPosX;
    json["hudPosY"] = hudPosY;
    json["isHudModule"] = true;
    json["m_size"] = m_size;
    json["m_backgroundOpacity"] = m_backgroundOpacity;
    json["m_background"] = m_background;
    json["m_showHours"] = m_showHours;
}
