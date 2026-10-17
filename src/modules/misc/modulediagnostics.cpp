#include "modulediagnostics.hpp"
#include "../../core/Diagnostics.hpp"
#include "../../config/AtomicConfigWriter.hpp"
#include <pl/ModMenu.hpp>

#include "modules/ModuleRegistry.hpp"
#include "config/ConfigManager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
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


std::string localClockLabel() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    if (localtime_r(&now, &local) == nullptr) return "--:--";
    char buffer[8]{};
    if (std::strftime(buffer, sizeof(buffer), "%H:%M", &local) == 0) return "--:--";
    return buffer;
}

std::string intervalLabel(int seconds) {
    if (seconds < 60) return std::to_string(seconds) + " s";
    if (seconds % 60 == 0) return std::to_string(seconds / 60) + " min";
    return std::to_string(seconds / 60) + "m " + std::to_string(seconds % 60) + "s";
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
        .category("diagnostics", "Diagnostics", "Module health report, Downloads export, and periodic runtime logs.");

    ConfigNodeV2 exportNode;
    exportNode.id = "m_exportLogsNow";
    exportNode.key = "m_exportLogsNow";
    exportNode.title = "Export all logs to Downloads now";
    exportNode.description = "Creates a timestamped file in Download/Bedaim using the selected format.";
    exportNode.category = "diagnostics";
    exportNode.type = ConfigControlTypeV2::Toggle;
    exportNode.defaultValue = "false";
    exportNode.currentValue = "false";
    exportNode.hasCurrentValue = true;
    schema.node(std::move(exportNode));

    ConfigNodeV2 formatNode;
    formatNode.id = "m_exportFormat";
    formatNode.key = "m_exportFormat";
    formatNode.title = "Export format";
    formatNode.description = "Choose readable plain text or structured JSON.";
    formatNode.category = "diagnostics";
    formatNode.type = ConfigControlTypeV2::Choice;
    formatNode.choiceStyle = ConfigChoiceStyleV2::Segmented;
    formatNode.defaultValue = "txt";
    formatNode.currentValue = m_exportFormat;
    formatNode.hasCurrentValue = true;
    ConfigOptionV2 txtOption;
    txtOption.value = "txt";
    txtOption.label = "TXT";
    formatNode.options.push_back(std::move(txtOption));
    ConfigOptionV2 jsonOption;
    jsonOption.value = "json";
    jsonOption.label = "JSON";
    formatNode.options.push_back(std::move(jsonOption));
    schema.node(std::move(formatNode));

    ConfigNodeV2 autoNode;
    autoNode.id = "m_autoExportEnabled";
    autoNode.key = "m_autoExportEnabled";
    autoNode.title = "Automatically export logs";
    autoNode.description = "While Module Diagnostics is enabled, periodically refreshes one rolling file in Download/Bedaim without blocking the game thread.";
    autoNode.category = "diagnostics";
    autoNode.type = ConfigControlTypeV2::Toggle;
    autoNode.defaultValue = "true";
    autoNode.currentValue = m_autoExportEnabled ? "true" : "false";
    autoNode.hasCurrentValue = true;
    schema.node(std::move(autoNode));

    ConfigNodeV2 intervalNode;
    intervalNode.id = "m_autoExportIntervalSeconds";
    intervalNode.key = "m_autoExportIntervalSeconds";
    intervalNode.title = "Automatic save interval";
    intervalNode.description = "Time between rolling auto-saves. Minimum 1 second, maximum 5 minutes. Defaults to every 5 seconds for useful diagnostics without excessive file writes.";
    intervalNode.category = "diagnostics";
    intervalNode.type = ConfigControlTypeV2::SliderFloat;
    intervalNode.minValue = "1";
    intervalNode.maxValue = "300";
    intervalNode.step = "1";
    intervalNode.unit = " s";
    intervalNode.defaultValue = "5";
    intervalNode.currentValue = std::to_string(m_autoExportIntervalSeconds);
    intervalNode.hasCurrentValue = true;
    schema.node(std::move(intervalNode));

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
        if (requested) exportLogs(false);
        return true;
    }
    if (key == "m_clearLogsNow") {
        if (requested) {
            // Do not add a confirmation entry immediately after clearing the
            // buffer; that would make "clear" leave a non-empty log.
            bedrocktools::diagnostics::clear();
        }
        return true;
    }
    if (key == "m_autoExportEnabled") {
        {
            std::lock_guard<std::mutex> lock(m_exportMutex);
            m_autoExportEnabled = requested;
        }
        m_exportCondition.notify_all();
        return true;
    }
    if (key == "m_exportFormat") {
        if (value != "txt" && value != "json") return false;
        {
            std::lock_guard<std::mutex> lock(m_exportMutex);
            m_exportFormat = std::string(value);
        }
        m_exportCondition.notify_all();
        return true;
    }
    if (key == "m_autoExportIntervalSeconds") {
        try {
            std::size_t parsedLength = 0;
            const std::string text(value);
            const float parsed = std::stof(text, &parsedLength);
            if (parsedLength != text.size() || !std::isfinite(parsed)) return false;
            {
                std::lock_guard<std::mutex> lock(m_exportMutex);
                m_autoExportIntervalSeconds = std::clamp(
                    static_cast<int>(std::lround(parsed)), 1, 300);
            }
            m_exportCondition.notify_all();
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

void ModuleDiagnosticsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    {
        std::lock_guard<std::mutex> lock(m_exportMutex);
        m_exportLogsNow = false;
        m_clearLogsNow = false;
        m_autoExportEnabled = j.value("m_autoExportEnabled", m_autoExportEnabled);
        if (j.contains("m_autoExportIntervalSeconds")) {
            m_autoExportIntervalSeconds = std::clamp(
                j.value("m_autoExportIntervalSeconds", m_autoExportIntervalSeconds), 1, 300);
        } else if (j.contains("m_autoExportIntervalMinutes")) {
            // Migrate settings from older builds, which stored the interval in minutes.
            const int oldMinutes = std::clamp(j.value("m_autoExportIntervalMinutes", 1), 1, 60);
            m_autoExportIntervalSeconds = std::clamp(oldMinutes * 60, 1, 300);
        }
        const auto format = j.value("m_exportFormat", m_exportFormat);
        m_exportFormat = (format == "json") ? "json" : "txt";
    }
    m_exportCondition.notify_all();
}

void ModuleDiagnosticsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_exportMutex);
    j["m_exportLogsNow"] = false;
    j["m_clearLogsNow"] = false;
    j["m_autoExportEnabled"] = m_autoExportEnabled;
    j["m_autoExportIntervalSeconds"] = m_autoExportIntervalSeconds;
    j["m_exportFormat"] = m_exportFormat;
}

ModuleDiagnosticsModule::~ModuleDiagnosticsModule() {
    stopExportWorker();
}

void ModuleDiagnosticsModule::onEnable() {
    m_summary = runDiagnostics();
    m_reportReady = true;
    m_nextRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    startExportWorker();
}

void ModuleDiagnosticsModule::onDisable() {
    stopExportWorker();
    m_reportReady = false;
    submitDrawCommands(moduleId, {});
}

void ModuleDiagnosticsModule::startExportWorker() {
    std::lock_guard<std::mutex> lock(m_exportMutex);
    if (m_exportWorker.joinable()) return;
    m_stopExportWorker = false;
    m_exportWorker = std::thread(&ModuleDiagnosticsModule::exportWorkerLoop, this);
}

void ModuleDiagnosticsModule::stopExportWorker() {
    {
        std::lock_guard<std::mutex> lock(m_exportMutex);
        m_stopExportWorker = true;
    }
    m_exportCondition.notify_all();
    if (m_exportWorker.joinable() && m_exportWorker.get_id() != std::this_thread::get_id())
        m_exportWorker.join();
}

void ModuleDiagnosticsModule::exportWorkerLoop() {
    std::unique_lock<std::mutex> lock(m_exportMutex);
    while (!m_stopExportWorker) {
        m_exportCondition.wait(lock, [this] {
            return m_stopExportWorker || m_autoExportEnabled;
        });
        if (m_stopExportWorker) break;

        // Save one snapshot immediately, then continue on the configured cadence.
        // Filesystem I/O stays off the game/render thread.
        lock.unlock();
        exportLogs(true);
        lock.lock();
        if (m_stopExportWorker) break;
        if (!m_autoExportEnabled) continue;

        const int intervalSeconds = m_autoExportIntervalSeconds;
        const bool changed = m_exportCondition.wait_for(
            lock, std::chrono::seconds(intervalSeconds), [this, intervalSeconds] {
                return m_stopExportWorker || !m_autoExportEnabled ||
                       m_autoExportIntervalSeconds != intervalSeconds;
            });
        if (m_stopExportWorker) break;
        if (changed) continue;
    }
}

bool ModuleDiagnosticsModule::exportLogs(bool automatic) {
    std::string format;
    {
        std::lock_guard<std::mutex> lock(m_exportMutex);
        format = m_exportFormat;
    }

    const std::filesystem::path outputDirectory("/sdcard/Download/Bedaim");
    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);
    if (ec || !std::filesystem::is_directory(outputDirectory, ec) || ec) {
        const std::string reason = "Cannot access /sdcard/Download/Bedaim; check Android storage access";
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error, "Diagnostics", reason);
        std::lock_guard<std::mutex> lock(m_exportMutex);
        m_lastExportStatus = "Task failed (" + localClockLabel() + "): Downloads unavailable";
        return false;
    }

    std::string filename;
    if (automatic) {
        filename = "bedaim_diagnostics_auto." + format;
    } else {
        const auto now = std::chrono::system_clock::now();
        const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        const std::time_t epoch = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_r(&epoch, &utc);
        char timestamp[24]{};
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &utc);
        const auto millis = static_cast<int>((epochMs % 1000 + 1000) % 1000);
        filename = "bedaim_diagnostics_" + std::string(timestamp) + "_" +
            (millis < 100 ? (millis < 10 ? "00" : "0") : "") +
            std::to_string(millis) + "." + format;
    }

    const auto outputPath = (outputDirectory / filename).string();
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Diagnostics",
        std::string(automatic ? "periodic" : "manual") + " log export requested; format=" + format);
    std::string error;
    const bool ok = format == "json"
        ? bedrocktools::diagnostics::exportJsonTo(outputPath, &error)
        : bedrocktools::diagnostics::exportTo(outputPath, &error);
    bedrocktools::diagnostics::log(ok ? bedrocktools::diagnostics::Level::Info :
        bedrocktools::diagnostics::Level::Error, "Diagnostics",
        ok ? "diagnostic logs exported to " + outputPath :
             "diagnostic log export failed: " + error);

    {
        std::lock_guard<std::mutex> lock(m_exportMutex);
        const std::string completedAt = localClockLabel();
        if (ok) {
            m_lastExportStatus = automatic
                ? "Task completed successfully (" + completedAt + ")"
                : "Manual export completed (" + completedAt + ")";
            m_lastExportPath = outputPath;
            m_lastSuccessfulExportTime = completedAt;
        } else {
            m_lastExportStatus = "Task failed (" + completedAt + "): " + error;
            // Keep the last known-good path/time visible when a later save fails.
        }
    }
    return ok;
}

void ModuleDiagnosticsModule::onFrame() {
    if (!enabled) return;

    const auto now = std::chrono::steady_clock::now();
    if (!m_reportReady || now >= m_nextRefresh) {
        m_summary = runDiagnostics();
        m_reportReady = true;
        m_nextRefresh = now + std::chrono::seconds(3);
    }

    std::string exportStatus;
    std::string exportPath;
    bool autoExport;
    int interval;
    std::string lastSuccessfulTime;
    {
        std::lock_guard<std::mutex> lock(m_exportMutex);
        exportStatus = m_lastExportStatus;
        exportPath = m_lastExportPath;
        autoExport = m_autoExportEnabled;
        interval = m_autoExportIntervalSeconds;
        lastSuccessfulTime = m_lastSuccessfulExportTime;
    }

    std::vector<PLModMenu_DrawCommand> commands;
    const std::string line1 = "Module Diagnostics: " + std::to_string(m_summary.total) +
        " modules | " + std::to_string(m_summary.enabled) + " enabled";
    const std::string line2 = "Logs: " + std::to_string(bedrocktools::diagnostics::count()) +
        " | Registry warn: " + std::to_string(m_summary.warnings) +
        " | err: " + std::to_string(m_summary.errors);
    const std::string line3 = m_summary.written
        ? "Health report: module_diagnostics.json"
        : "Health report write failed (check config folder)";
    const std::string line4 = autoExport
        ? "Auto-save: every " + intervalLabel(interval) + " | " + exportStatus
        : "Auto-save off | " + exportStatus;
    const std::string line5 = "Last successful save: " + lastSuccessfulTime +
        (exportPath.empty() ? " | Folder: Download/Bedaim" : " | " + exportPath);

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
    addText(line4, 56.0f, exportStatus.find("failed") != std::string::npos ? 0xFFFF7070 : 0xFFFFFFFF);
    addText(line5, 72.0f, 0xFFFFFFFF);
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

    // Include a compact tail of runtime events in the JSON report so the report
    // can show both registry health and the most recent observed failure gates.
    const auto runtimeEvents = bedrocktools::diagnostics::snapshot();
    const std::size_t firstRuntimeEvent = runtimeEvents.size() > 128
        ? runtimeEvents.size() - 128 : 0;
    report["runtime_log"] = {
        {"buffered_entries", runtimeEvents.size()},
        {"capacity", 4096},
        {"recent_entries", json::array()}
    };
    for (std::size_t i = firstRuntimeEvent; i < runtimeEvents.size(); ++i) {
        const auto& entry = runtimeEvents[i];
        report["runtime_log"]["recent_entries"].push_back({
            {"timestamp_ms", entry.timestampMs},
            {"sequence", entry.sequence},
            {"level", bedrocktools::diagnostics::levelName(entry.level)},
            {"component", entry.component},
            {"message", entry.message}
        });
    }

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
    const std::string payload = report.dump(2) + "\n";
    summary.written = bedrocktools::config::writePayloadAtomically(
        outputPath.string(), payload);
    return summary;
}
