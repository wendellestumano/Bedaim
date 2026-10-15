#include "distancetracker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <pl/ModMenu.hpp>

namespace {
std::string formatBlocks(double blocks) {
    char buffer[48]{};
    std::snprintf(buffer, sizeof(buffer), "%.1f", blocks);
    return buffer;
}
}

DistanceTrackerModule::DistanceTrackerModule()
    : Module("Distance Tracker", "Tracks distance traveled while enabled and filters large teleport jumps.") {}

void DistanceTrackerModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) {
            if (!enabled || !event.player) return;
            const auto position = event.player->position();
            std::lock_guard lock(m_mutex);
            m_accumulator.update(position.x, position.y, position.z, m_use3D, m_maxStepBlocks);
        });
}

void DistanceTrackerModule::onEnable() {
    std::lock_guard lock(m_mutex);
    m_accumulator.reset();
}

void DistanceTrackerModule::onDisable() {
    std::lock_guard lock(m_mutex);
    m_accumulator.reset();
}

void DistanceTrackerModule::onFrame() {
    if (!enabled) return;
    float x = 0.0f, y = 0.0f, size = 18.0f, opacity = 0.55f;
    bool background = true, use3D = false;
    double totalBlocks = 0.0;
    {
        std::lock_guard lock(m_mutex);
        x = hudPosX; y = hudPosY; size = m_size; opacity = m_backgroundOpacity;
        background = m_background; use3D = m_use3D;
        totalBlocks = m_accumulator.totalBlocks();
    }

    const std::string text = "Distance: " + formatBlocks(totalBlocks) +
                             (use3D ? " 3D blocks" : " blocks");
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

void DistanceTrackerModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("tracking")
        .category("tracking", "Tracking", "Distance accumulation behavior")
        .category("appearance", "Appearance", "Overlay size and background");

    ConfigNodeV2 mode;
    mode.id = "m_use3D"; mode.key = "m_use3D"; mode.title = "Count vertical movement";
    mode.category = "tracking"; mode.description = "Include Y-axis movement in the distance total.";
    mode.type = ConfigControlTypeV2::Toggle;
    schema.node(std::move(mode));

    ConfigNodeV2 maxStep;
    maxStep.id = "m_maxStepBlocks"; maxStep.key = "m_maxStepBlocks";
    maxStep.title = "Teleport filter threshold"; maxStep.category = "tracking";
    maxStep.description = "Ignore a single-tick displacement larger than this many blocks.";
    maxStep.type = ConfigControlTypeV2::SliderFloat;
    maxStep.minValue = "4"; maxStep.maxValue = "64"; maxStep.step = "1"; maxStep.unit = " blocks";
    schema.node(std::move(maxStep));

    ConfigNodeV2 background;
    background.id = "m_background"; background.key = "m_background";
    background.title = "Background"; background.category = "appearance";
    background.type = ConfigControlTypeV2::Toggle;
    schema.node(std::move(background));

    ConfigNodeV2 size;
    size.id = "m_size"; size.key = "m_size"; size.title = "Text size";
    size.category = "appearance"; size.type = ConfigControlTypeV2::SliderFloat;
    size.minValue = "8"; size.maxValue = "48"; size.step = "1"; size.unit = " px";
    schema.node(std::move(size));

    ConfigNodeV2 opacity;
    opacity.id = "m_backgroundOpacity"; opacity.key = "m_backgroundOpacity";
    opacity.title = "Background opacity"; opacity.category = "appearance";
    opacity.type = ConfigControlTypeV2::SliderFloat;
    opacity.minValue = "0"; opacity.maxValue = "1"; opacity.step = "0.05";
    opacity.visibleWhen = {{"m_background", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(opacity));

    ConfigNodeV2 keybind;
    keybind.id = "keybind"; keybind.key = "keybind"; keybind.title = "Keybind";
    keybind.category = "tracking"; keybind.type = ConfigControlTypeV2::Keybind;
    schema.node(std::move(keybind));
    setConfigSchemaJson(moduleId, schema.toJson());
}

void DistanceTrackerModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard lock(m_mutex);
    if (json.contains("hudPosX") && json["hudPosX"].is_number()) hudPosX = std::clamp(json["hudPosX"].get<float>(), 0.0f, 4000.0f);
    if (json.contains("hudPosY") && json["hudPosY"].is_number()) hudPosY = std::clamp(json["hudPosY"].get<float>(), 0.0f, 4000.0f);
    if (json.contains("m_size") && json["m_size"].is_number()) m_size = std::clamp(json["m_size"].get<float>(), 8.0f, 48.0f);
    if (json.contains("m_backgroundOpacity") && json["m_backgroundOpacity"].is_number()) m_backgroundOpacity = std::clamp(json["m_backgroundOpacity"].get<float>(), 0.0f, 1.0f);
    if (json.contains("m_maxStepBlocks") && json["m_maxStepBlocks"].is_number()) m_maxStepBlocks = std::clamp(json["m_maxStepBlocks"].get<float>(), 4.0f, 64.0f);
    if (json.contains("m_use3D") && json["m_use3D"].is_boolean()) m_use3D = json["m_use3D"].get<bool>();
    if (json.contains("m_background") && json["m_background"].is_boolean()) m_background = json["m_background"].get<bool>();
}

void DistanceTrackerModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard lock(m_mutex);
    json["hudPosX"] = hudPosX; json["hudPosY"] = hudPosY; json["isHudModule"] = true;
    json["m_size"] = m_size; json["m_backgroundOpacity"] = m_backgroundOpacity;
    json["m_maxStepBlocks"] = m_maxStepBlocks; json["m_use3D"] = m_use3D;
    json["m_background"] = m_background;
}
