#include "modulediagnostics.hpp"
#include "../../core/Diagnostics.hpp"
#include <pl/ModMenu.hpp>

#include "modules/ModuleRegistry.hpp"
#include "config/ConfigManager.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using json = nlohmann::json;

std::string safeText(const char* value) {
    return value ? std::string(value) : std::string{};
}

std::string moduleStatus(const Module* module) {
    if (!module) return "invalid";
    if (module->enabled) return "enabled";
    if (module->masterEnabled && !module->keybindActive) return "blocked_by_keybind";
    if (!module->masterEnabled) return "disabled";
    return "inactive";
}
}

ModuleDiagnosticsModule::ModuleDiagnosticsModule()
    : Module("Module Diagnostics",
             "Generates a JSON health report for registered modules and shows a compact status summary, and exports all diagnostic log levels.") {
    keybind = 0;
}

void ModuleDiagnosticsModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("diagnostics")
        .category("diagnostics", "Diagnostics", "Module health report and exportable runtime logs.");

    ConfigNodeV2 exportNode;
    exportNode.id = "m_exportLogsNow";
    exportNode.key = "m_exportLogsNow";
    exportNode.title = "Export all logs now";
    exportNode.description = "Writes bedaim_diagnostics.log next to config.json, including trace/debug/info/warn/error/critical entries.";
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
    clearNode.description = "Clears the 4096-entry log buffer. Export first if you need to keep it.";
    clearNode.category = "diagnostics";
    clearNode.type = ConfigControlTypeV2::Toggle;
    clearNode.defaultValue = "false";
    clearNode.currentValue = "false";
    clearNode.hasCurrentValue = true;
    schema.node(std::move(clearNode));
    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

bool ModuleDiagnosticsModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    const bool requested = value == "true" || value == "1" || value == "on";
    if (key == "m_exportLogsNow") {
        if (!requested) return true;
        const auto configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
        const auto outputPath = (std::filesystem::path(configPath).parent_path() /
                                 "bedaim_diagnostics.log").string();
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Diagnostics",
            "log export requested; destination=" + outputPath);
        std::string error;
        const bool ok = bedrocktools::diagnostics::exportTo(outputPath, &error);
        bedrocktools::diagnostics::log(ok ? bedrocktools::diagnostics::Level::Info :
            bedrocktools::diagnostics::Level::Error, "Diagnostics",
            ok ? "diagnostic log exported to " + outputPath :
                 "diagnostic log export failed: " + error);
        return true;
    }
    if (key == "m_clearLogsNow") {
        if (requested) {
            bedrocktools::diagnostics::clear();
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Diagnostics",
                "in-memory diagnostic log cleared from settings");
        }
        return true;
    }
    return false;
}

void ModuleDiagnosticsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_exportLogsNow = false;
    m_clearLogsNow = false;
}

void ModuleDiagnosticsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_exportLogsNow"] = false;
    j["m_clearLogsNow"] = false;
}

void ModuleDiagnosticsModule::onEnable() {
    m_summary = runDiagnostics();
    m_reportReady = true;
    m_nextRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

void ModuleDiagnosticsModule::onDisable() {
    m_reportReady = false;
    submitDrawCommands(moduleId, {});
}

void ModuleDiagnosticsModule::onFrame() {
    if (!enabled) return;

    const auto now = std::chrono::steady_clock::now();
    if (!m_reportReady || now >= m_nextRefresh) {
        m_summary = runDiagnostics();
        m_reportReady = true;
        m_nextRefresh = now + std::chrono::seconds(3);
    }

    std::vector<PLModMenu_DrawCommand> commands;
    const std::string line1 = "Module Diagnostics: " + std::to_string(m_summary.total) +
        " modules | " + std::to_string(m_summary.enabled) + " enabled";
    const std::string line2 = "Logs: " + std::to_string(bedrocktools::diagnostics::count()) +
        " | Warnings: " + std::to_string(m_summary.warnings) +
        " | Errors: " + std::to_string(m_summary.errors);
    const std::string line3 = m_summary.written
        ? "Report saved: module_diagnostics.json"
        : "Report write failed (check config folder)";

    auto addText = [&](const std::string& text, float y, std::uint32_t color) {
        PLModMenu_DrawCommand command{};
        command.type = PL_DRAW_TEXT;
        command.x = 8.0f;
        command.y = y;
        command.w = 480.0f;
        command.h = 16.0f;
        command.size = 12.0f;
        command.color = color;
        command.text = text.c_str();
        commands.push_back(command);
    };

    addText(line1, 8.0f, 0xFFFFFFFF);
    addText(line2, 24.0f, (m_summary.errors || m_summary.warnings) ? 0xFFFFC060 : 0xFF80FF80);
    addText(line3, 40.0f, m_summary.written ? 0xFF80FF80 : 0xFFFF7070);
    submitDrawCommands(moduleId, commands);
}

ModuleDiagnosticsModule::ReportSummary ModuleDiagnosticsModule::runDiagnostics() {
    ReportSummary summary{};
    auto& registry = ModuleRegistry::get();
    const auto& modules = registry.modules();
    summary.total = modules.size();

    std::unordered_map<std::string, std::size_t> idCounts;
    idCounts.reserve(modules.size());
    for (const auto* module : modules) {
        if (!module) continue;
        ++idCounts[module->moduleId];
        if (module->enabled) ++summary.enabled;
    }

    json report;
    report["schema_version"] = 1;
    report["generated_at_unix"] = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    report["summary"] = {
        {"total_modules", summary.total},
        {"enabled_modules", summary.enabled},
        {"warnings", 0},
        {"errors", 0}
    };
    report["checks"] = json::array();
    report["modules"] = json::array();

    auto addCheck = [&](const std::string& id, const std::string& status,
                        const std::string& detail) {
        report["checks"].push_back({{"id", id}, {"status", status}, {"detail", detail}});
        if (status == "warning") ++summary.warnings;
        else if (status == "error") ++summary.errors;
    };

    if (modules.empty()) {
        addCheck("registry_nonempty", "error", "No modules are registered.");
    } else {
        addCheck("registry_nonempty", "ok", "Registered module list is not empty.");
    }

    std::unordered_set<std::string> seenIds;
    seenIds.reserve(modules.size());
    for (const auto* module : modules) {
        if (!module) {
            addCheck("null_module", "error", "ModuleRegistry contains a null module pointer.");
            report["modules"].push_back({
                {"name", "<null>"}, {"id", ""}, {"status", "invalid"},
                {"warnings", json::array({"null module pointer"})}
            });
            continue;
        }

        json warnings = json::array();
        const std::string name = safeText(module->name);
        const std::string description = safeText(module->description);
        const std::string id = module->moduleId;

        if (name.empty()) {
            warnings.push_back("empty module name");
            addCheck("module_name", "warning", "A registered module has an empty name.");
        }
        if (description.empty()) {
            warnings.push_back("empty module description");
            addCheck("module_description", "warning", "Module '" + (name.empty() ? id : name) + "' has no description.");
        }
        if (id.empty()) {
            warnings.push_back("empty module id");
            addCheck("module_id", "error", "A registered module has an empty ID.");
        }
        if (!seenIds.insert(id).second || idCounts[id] > 1) {
            warnings.push_back("duplicate module id");
            addCheck("duplicate_module_id", "error", "Duplicate module ID: " + id);
        }

        const bool expectedEnabled = module->masterEnabled && module->keybindActive;
        if (module->enabled != expectedEnabled) {
            warnings.push_back("enabled state differs from masterEnabled && keybindActive");
            addCheck("activation_state", "warning",
                     "Activation state mismatch for '" + (name.empty() ? id : name) + "'.");
        }

        report["modules"].push_back({
            {"name", name},
            {"id", id},
            {"description", description},
            {"status", moduleStatus(module)},
            {"enabled", module->enabled},
            {"master_enabled", module->masterEnabled},
            {"keybind_active", module->keybindActive},
            {"keybind", module->keybind},
            {"show_in_menu", module->showInMenu},
            {"warnings", std::move(warnings)}
        });
    }

    if (summary.warnings == 0 && summary.errors == 0) {
        addCheck("registry_consistency", "ok",
                 "No generic registry or activation-state issues were detected.");
    }

    report["summary"]["warnings"] = summary.warnings;
    report["summary"]["errors"] = summary.errors;
    report["limitations"] = json::array({
        "This report checks registration, metadata, duplicate IDs, and activation-state consistency.",
        "It cannot prove that a module's game hooks or behavior work at runtime; module-specific probes and in-game tests are required."
    });

    const std::filesystem::path configPath =
        bedrocktools::config::ConfigManager::get().getConfigPath();
    const std::filesystem::path outputPath =
        configPath.empty() ? std::filesystem::path{} :
        std::filesystem::path(configPath).parent_path() / "module_diagnostics.json";
    if (outputPath.empty()) return summary;

    summary.outputPath = outputPath.string();
    const std::filesystem::path tempPath = outputPath.string() + ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(outputPath.parent_path(), ec);
    if (ec) return summary;

    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output) return summary;
        const std::string payload = report.dump(2) + "\n";
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.flush();
        if (!output.good()) {
            output.close();
            std::filesystem::remove(tempPath, ec);
            return summary;
        }
    }

    std::filesystem::rename(tempPath, outputPath, ec);
    if (ec) {
        // Some filesystems do not replace an existing file atomically; fall back to
        // a remove-and-rename sequence while keeping the temporary report intact.
        std::error_code removeError;
        std::filesystem::remove(outputPath, removeError);
        ec.clear();
        std::filesystem::rename(tempPath, outputPath, ec);
    }
    summary.written = !ec;
    if (ec) std::filesystem::remove(tempPath, ec);
    return summary;
}
