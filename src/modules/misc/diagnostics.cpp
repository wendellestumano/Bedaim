#include "diagnostics.hpp"
#include "../core/Diagnostics.hpp"
#include "../config/ConfigManager.hpp"

#include <pl/ModMenu.hpp>
#include <filesystem>
#include <string>
#include <utility>

DiagnosticsModule::DiagnosticsModule()
    : Module("Module Diagnostics", "Captures Bedaim trace, debug, info, warning, error and critical events; exports a shareable log.", "Module Diagnostics") {
    keybind = 0;
}

void DiagnosticsModule::onInit() {
    bedrocktools::diagnostics::log(
        bedrocktools::diagnostics::Level::Info, "Diagnostics",
        "Module Diagnostics initialized; all Bedaim diagnostic levels are captured.");
}

void DiagnosticsModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("diagnostics")
        .category("diagnostics", "Diagnostics", "Export a log file you can share for debugging.");

    ConfigNodeV2 exportNode;
    exportNode.id = "m_exportLogsNow";
    exportNode.key = "m_exportLogsNow";
    exportNode.title = "Export all logs now";
    exportNode.description = "Writes bedaim_diagnostics.log next to config.json. The toggle resets after the export attempt.";
    exportNode.category = "diagnostics";
    exportNode.type = ConfigControlTypeV2::Toggle;
    exportNode.defaultValue = "false";
    exportNode.currentValue = "false";
    exportNode.hasCurrentValue = true;
    schema.node(std::move(exportNode));

    ConfigNodeV2 clearNode;
    clearNode.id = "m_clearLogsNow";
    clearNode.key = "m_clearLogsNow";
    clearNode.title = "Clear in-memory logs";
    clearNode.description = "Clears the current 4096-entry diagnostic buffer. Export first if you need to keep it.";
    clearNode.category = "diagnostics";
    clearNode.type = ConfigControlTypeV2::Toggle;
    clearNode.defaultValue = "false";
    clearNode.currentValue = "false";
    clearNode.hasCurrentValue = true;
    schema.node(std::move(clearNode));

    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

bool DiagnosticsModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    const bool enabled = value == "true" || value == "1" || value == "on";
    if (key == "m_exportLogsNow") {
        if (!enabled) {
            m_exportLogsNow = false;
            return true;
        }
        m_exportLogsNow = false;
        const auto configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
        const auto exportPath = (std::filesystem::path(configPath).parent_path() /
                                 "bedaim_diagnostics.log").string();
        std::string error;
        const bool ok = bedrocktools::diagnostics::exportTo(exportPath, &error);
        bedrocktools::diagnostics::log(
            ok ? bedrocktools::diagnostics::Level::Info : bedrocktools::diagnostics::Level::Error,
            "Diagnostics",
            ok ? "Exported diagnostic log to " + exportPath : "Diagnostic log export failed: " + error);
        return true;
    }
    if (key == "m_clearLogsNow") {
        m_clearLogsNow = false;
        if (enabled) {
            bedrocktools::diagnostics::clear();
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Info, "Diagnostics",
                "In-memory diagnostic buffer cleared from settings.");
        }
        return true;
    }
    return false;
}

void DiagnosticsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    // Action toggles are intentionally not replayed on startup.
    m_exportLogsNow = false;
    m_clearLogsNow = false;
}

void DiagnosticsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_exportLogsNow"] = false;
    j["m_clearLogsNow"] = false;
}
