#include "configurations.hpp"
#include "../../config/ConfigManager.hpp"
#include "../ModuleRegistry.hpp"
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace {
std::string trimFileName(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "Configs.json";
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    // This control is a filename, not an arbitrary path.
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return c == '/' || c == '\\' || c == ':' || c < 32;
    }), value.end());
    if (value.empty() || value == "." || value == "..") return "Configs.json";
    if (value.size() < 5 || value.substr(value.size() - 5) != ".json") value += ".json";
    if (value.size() > 96) value = value.substr(0, 91) + ".json";
    return value;
}

bool parseToggle(std::string_view value, bool& result) {
    if (value == "true" || value == "1" || value == "on") { result = true; return true; }
    if (value == "false" || value == "0" || value == "off") { result = false; return true; }
    return false;
}
}

ConfigurationsModule::ConfigurationsModule()
    : Module("Configurations", "Periodically saves every Bedaim module's configuration and optionally saves when settings change.") {
    keybind = 0;
}

void ConfigurationsModule::onEnable() {
    m_nextSave = std::chrono::steady_clock::now() + std::chrono::seconds(intervalSeconds);
    m_lastSnapshot = currentSnapshot();
    m_hasSnapshot = true;
}

void ConfigurationsModule::onDisable() {
    m_hasSnapshot = false;
}

std::string ConfigurationsModule::outputPath() const {
    const std::filesystem::path primary(bedrocktools::config::ConfigManager::get().getConfigPath());
    return (primary.parent_path() / trimFileName(fileName)).string();
}

nlohmann::json ConfigurationsModule::currentSnapshot() const {
    nlohmann::json snapshot;
    for (auto* module : ModuleRegistry::get().modules()) {
        if (!module) continue;
        nlohmann::json moduleConfig;
        module->saveConfig(moduleConfig);
        snapshot["Modules"][module->name] = std::move(moduleConfig);
    }
    return snapshot;
}

void ConfigurationsModule::saveAll() {
    auto& manager = bedrocktools::config::ConfigManager::get();
    // Keep the canonical config used during startup current, and also write the
    // named snapshot requested by this module. Both writes use the async atomic writer.
    manager.save();
    manager.saveTo(outputPath());
    m_lastSnapshot = currentSnapshot();
    m_hasSnapshot = true;
    m_nextSave = std::chrono::steady_clock::now() + std::chrono::seconds(intervalSeconds);
}

void ConfigurationsModule::onFrame() {
    const auto now = std::chrono::steady_clock::now();
    if (changeDetection) {
        const auto snapshot = currentSnapshot();
        if (!m_hasSnapshot) {
            m_lastSnapshot = snapshot;
            m_hasSnapshot = true;
        } else if (snapshot != m_lastSnapshot) {
            saveAll();
            return;
        }
    }
    if (m_nextSave.time_since_epoch().count() == 0) {
        m_nextSave = now + std::chrono::seconds(intervalSeconds);
        return;
    }
    if (now >= m_nextSave) saveAll();
}

void ConfigurationsModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("configurations")
        .category("configurations", "Configurations", "Automatic saves for all Bedaim module settings.");

    ConfigNodeV2 interval;
    interval.id = "intervalSeconds";
    interval.key = "intervalSeconds";
    interval.title = "Save interval";
    interval.description = "Time between automatic saves. Range: 5 seconds to 5 minutes.";
    interval.category = "configurations";
    interval.type = ConfigControlTypeV2::SliderFloat;
    interval.defaultValue = "5";
    interval.currentValue = std::to_string(intervalSeconds);
    interval.hasCurrentValue = true;
    interval.minValue = "5";
    interval.maxValue = "300";
    interval.step = "5";
    interval.unit = " s";
    schema.node(std::move(interval));

    ConfigNodeV2 name;
    name.id = "fileName";
    name.key = "fileName";
    name.title = "Configuration filename";
    name.description = "Name of the saved snapshot in the active BEDAIM configuration folder. Defaults to Configs.json.";
    name.category = "configurations";
    name.type = ConfigControlTypeV2::Text;
    name.defaultValue = "Configs.json";
    name.currentValue = fileName;
    name.hasCurrentValue = true;
    name.maxLength = 96;
    name.placeholder = "Configs.json";
    schema.node(std::move(name));

    ConfigNodeV2 detection;
    detection.id = "changeDetection";
    detection.key = "changeDetection";
    detection.title = "Change detection";
    detection.description = "When enabled, compares module settings and saves all configurations as soon as a change is detected. Disabled by default.";
    detection.category = "configurations";
    detection.type = ConfigControlTypeV2::Toggle;
    detection.defaultValue = "false";
    detection.currentValue = changeDetection ? "true" : "false";
    detection.hasCurrentValue = true;
    schema.node(std::move(detection));

    setConfigSchemaJson(moduleId, schema.toJson());
}

bool ConfigurationsModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "intervalSeconds") {
        try {
            std::size_t used = 0;
            const std::string text(value);
            const float parsed = std::stof(text, &used);
            if (used != text.size() || !std::isfinite(parsed)) return false;
            intervalSeconds = std::clamp(static_cast<int>(std::lround(parsed / 5.0f) * 5), 5, 300);
            m_nextSave = std::chrono::steady_clock::now() + std::chrono::seconds(intervalSeconds);
            return true;
        } catch (...) { return false; }
    }
    if (key == "fileName") {
        fileName = trimFileName(std::string(value));
        return true;
    }
    if (key == "changeDetection") {
        bool parsed = false;
        if (!parseToggle(value, parsed)) return false;
        changeDetection = parsed;
        m_lastSnapshot = currentSnapshot();
        m_hasSnapshot = true;
        return true;
    }
    return false;
}

void ConfigurationsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    intervalSeconds = std::clamp(j.value("intervalSeconds", intervalSeconds), 5, 300);
    intervalSeconds = std::clamp(((intervalSeconds + 2) / 5) * 5, 5, 300);
    fileName = trimFileName(j.value("fileName", fileName));
    changeDetection = j.value("changeDetection", changeDetection);
    m_nextSave = std::chrono::steady_clock::now() + std::chrono::seconds(intervalSeconds);
    m_hasSnapshot = false;
}

void ConfigurationsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["intervalSeconds"] = intervalSeconds;
    j["fileName"] = trimFileName(fileName);
    j["changeDetection"] = changeDetection;
}
