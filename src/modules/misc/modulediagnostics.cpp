#include "modulediagnostics.hpp"
#include "../../core/Diagnostics.hpp"
#include "../../config/AtomicConfigWriter.hpp"
#include <pl/ModMenu.hpp>

#include "modules/ModuleRegistry.hpp"
#include "config/ConfigManager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <ctime>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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

bool mergeCurrentSessionIntoLifetime(const std::filesystem::path& currentPath,
                                     const std::filesystem::path& lifetimePath,
                                     std::string* error = nullptr) noexcept {
    try {
        std::error_code ec;
        if (!std::filesystem::exists(currentPath, ec)) {
            if (ec && error) *error = "cannot inspect current-session log: " + ec.message();
            return !ec;
        }

        std::ifstream currentStream(currentPath);
        if (!currentStream) {
            if (error) *error = "cannot open current-session log";
            return false;
        }
        std::string currentPayload((std::istreambuf_iterator<char>(currentStream)),
                                   std::istreambuf_iterator<char>());
        // Older builds accidentally appended the two literal characters \\n
        // after JSON. Normalize only that trailing artifact; never discard entries.
        while (!currentPayload.empty() &&
               std::isspace(static_cast<unsigned char>(currentPayload.back())))
            currentPayload.pop_back();
        while (currentPayload.size() >= 2 &&
               currentPayload.compare(currentPayload.size() - 2, 2, "\\n") == 0) {
            currentPayload.resize(currentPayload.size() - 2);
            while (!currentPayload.empty() &&
                   std::isspace(static_cast<unsigned char>(currentPayload.back())))
                currentPayload.pop_back();
        }
        json current = json::parse(currentPayload);
        if (!current.is_object() || !current.contains("entries") ||
            !current["entries"].is_array()) {
            if (error) *error = "current-session JSON has no entries array";
            return false;
        }

        json lifetime = json::object();
        if (std::filesystem::exists(lifetimePath, ec)) {
            if (ec) {
                if (error) *error = "cannot inspect lifetime log: " + ec.message();
                return false;
            }
            std::ifstream lifetimeStream(lifetimePath);
            if (!lifetimeStream) {
                if (error) *error = "cannot open lifetime log";
                return false;
            }
            std::string lifetimePayload((std::istreambuf_iterator<char>(lifetimeStream)),
                                        std::istreambuf_iterator<char>());
            while (!lifetimePayload.empty() &&
                   std::isspace(static_cast<unsigned char>(lifetimePayload.back())))
                lifetimePayload.pop_back();
            while (lifetimePayload.size() >= 2 &&
                   lifetimePayload.compare(lifetimePayload.size() - 2, 2, "\\n") == 0) {
                lifetimePayload.resize(lifetimePayload.size() - 2);
                while (!lifetimePayload.empty() &&
                       std::isspace(static_cast<unsigned char>(lifetimePayload.back())))
                    lifetimePayload.pop_back();
            }
            lifetime = json::parse(lifetimePayload);
            if (!lifetime.is_object() || !lifetime.contains("entries") ||
                !lifetime["entries"].is_array()) {
                if (error) *error = "lifetime JSON is invalid; refusing to overwrite it";
                return false;
            }
        } else {
            lifetime["schema_version"] = 1;
            lifetime["created_at_unix_ms"] =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            lifetime["entries"] = json::array();
            lifetime["session_ids"] = json::array();
        }

        if (!lifetime.contains("session_ids") || !lifetime["session_ids"].is_array())
            lifetime["session_ids"] = json::array();

        // This export adds monotonic time and thread identity; mark upgraded
        // lifetime files even when they were originally created by schema v1.
        lifetime["schema_version"] = 2;
        auto entryKey = [](const json& entry) {
            return std::to_string(entry.value("timestamp_ms", std::int64_t{0})) + "|" +
                std::to_string(entry.value("sequence", std::uint64_t{0})) + "|" +
                entry.value("level", std::string{}) + "|" +
                entry.value("component", std::string{}) + "|" +
                entry.value("message", std::string{}) + "|" +
                std::to_string(entry.value("monotonic_ns", std::uint64_t{0})) + "|" +
                std::to_string(entry.value("thread_id", std::uint64_t{0}));
        };

        std::unordered_set<std::string> existing;
        existing.reserve(lifetime["entries"].size() + current["entries"].size());
        for (const auto& entry : lifetime["entries"]) {
            if (entry.is_object()) existing.insert(entryKey(entry));
        }

        std::string sessionId = "unknown";
        if (!current["entries"].empty() && current["entries"].front().is_object())
            sessionId = std::to_string(current["entries"].front().value(
                "timestamp_ms", std::int64_t{0}));
        if (std::find(lifetime["session_ids"].begin(), lifetime["session_ids"].end(),
                      json(sessionId)) == lifetime["session_ids"].end()) {
            lifetime["session_ids"].push_back(sessionId);
        }

        std::size_t appended = 0;
        for (const auto& sourceEntry : current["entries"]) {
            if (!sourceEntry.is_object()) continue;
            const auto key = entryKey(sourceEntry);
            if (!existing.insert(key).second) continue;
            json archivedEntry = sourceEntry;
            archivedEntry["session_id"] = sessionId;
            lifetime["entries"].push_back(std::move(archivedEntry));
            ++appended;
        }

        lifetime["schema_version"] = 2;
        lifetime["generated_at_unix_ms"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        lifetime["entry_count"] = lifetime["entries"].size();
        lifetime["session_count"] = lifetime["session_ids"].size();
        lifetime["last_merge_appended"] = appended;

        if (!bedrocktools::config::writePayloadAtomically(
                lifetimePath.string(), lifetime.dump(2) + "\n")) {
            if (error) *error = "could not atomically replace lifetime log";
            return false;
        }
        if (error) error->clear();
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    } catch (...) {
        if (error) *error = "unknown lifetime log merge failure";
        return false;
    }
}


bool exportGameplayOnlyJsonTo(const std::string& path, std::string* error = nullptr) noexcept {
    try {
        if (path.empty()) {
            if (error) *error = "empty export path";
            return false;
        }
        const auto entries = bedrocktools::diagnostics::snapshot();
        const auto isGameplay = [](const bedrocktools::diagnostics::Entry& entry) {
            // Keep module decisions and the native routes they depend on, while
            // excluding generic startup, registry and exporter chatter.
            return entry.component != "Runtime" &&
                   entry.component != "Config" &&
                   entry.component != "ModuleRegistry" &&
                   entry.component != "Diagnostics";
        };
        json payload;
        payload["schema_version"] = 2;
        payload["mode"] = "GAMEPLAY_ONLY_LOGS";
        payload["generated_at_unix_ms"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        payload["buffer_capacity"] = bedrocktools::diagnostics::bufferCapacity();
        payload["dropped_entry_count"] = bedrocktools::diagnostics::droppedCount();
        payload["entries"] = json::array();
        for (const auto& entry : entries) {
            if (!isGameplay(entry)) continue;
            payload["entries"].push_back({
                {"timestamp_ms", entry.timestampMs},
                {"monotonic_ns", entry.monotonicNs},
                {"thread_id", entry.threadId},
                {"sequence", entry.sequence},
                {"level", bedrocktools::diagnostics::levelName(entry.level)},
                {"component", entry.component},
                {"message", entry.message}
            });
        }
        payload["entry_count"] = payload["entries"].size();
        payload["first_sequence"] = payload["entries"].empty()
            ? 0 : payload["entries"].front().value("sequence", std::uint64_t{0});
        payload["last_sequence"] = payload["entries"].empty()
            ? 0 : payload["entries"].back().value("sequence", std::uint64_t{0});
        if (!bedrocktools::config::writePayloadAtomically(path, payload.dump(2) + "\n")) {
            if (error) *error = "could not atomically replace gameplay-only JSON log";
            return false;
        }
        if (error) error->clear();
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    } catch (...) {
        if (error) *error = "unknown gameplay-only export failure";
        return false;
    }
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
    formatNode.description = "Choose the format for timestamped manual exports. Rolling current-session and lifetime logs are always JSON.";
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
    autoNode.description = "While enabled, refreshes the current-session JSON logs on the selected cadence. Cumulative history files are synced every 60 seconds and when rotating sessions.";
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
    intervalNode.description = "Time between rolling current-session saves. Minimum 5 seconds, maximum 5 minutes. Cumulative history files sync every 60 seconds to avoid repeatedly rewriting multi-megabyte archives.";
    intervalNode.category = "diagnostics";
    intervalNode.type = ConfigControlTypeV2::SliderFloat;
    intervalNode.minValue = "5";
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
    clearNode.description = "Clears the 32768-entry log buffer and dropped counter. Export first if you need to keep it.";
    clearNode.category = "diagnostics";
    clearNode.type = ConfigControlTypeV2::Toggle;
    clearNode.defaultValue = "false";
    clearNode.currentValue = "false";
    clearNode.hasCurrentValue = true;
    schema.node(std::move(clearNode));

    ConfigNodeV2 freshSessionNode;
    freshSessionNode.id = "m_startFreshLogSession";
    freshSessionNode.key = "m_startFreshLogSession";
    freshSessionNode.title = "Start fresh gameplay log session";
    freshSessionNode.description = "Clears previous gameplay entries, keeps essential startup/hook diagnostics, and atomically replaces the rolling auto-save. Timestamped manual exports are preserved.";
    freshSessionNode.category = "diagnostics";
    freshSessionNode.type = ConfigControlTypeV2::Toggle;
    freshSessionNode.defaultValue = "false";
    freshSessionNode.currentValue = "false";
    freshSessionNode.hasCurrentValue = true;
    schema.node(std::move(freshSessionNode));

    auto addOverlaySlider = [&](const std::string& id, const std::string& title,
                                int minimum, int maximum, int initial, const std::string& unit) {
        ConfigNodeV2 node;
        node.id = id;
        node.key = id;
        node.title = title;
        node.description = "Adjusts only the Module Diagnostics overlay; log collection and export settings are unchanged.";
        node.category = "diagnostics";
        node.type = ConfigControlTypeV2::SliderFloat;
        node.minValue = std::to_string(minimum);
        node.maxValue = std::to_string(maximum);
        node.step = "1";
        node.unit = unit;
        node.defaultValue = std::to_string(initial);
        node.currentValue = std::to_string(initial);
        node.hasCurrentValue = true;
        schema.node(std::move(node));
    };
    addOverlaySlider("m_overlayX", "Overlay X position", 0, 1800, 8, " px");
    addOverlaySlider("m_overlayY", "Overlay Y position", 0, 1000, 8, " px");
    addOverlaySlider("m_overlayWidth", "Overlay width", 240, 1400, 480, " px");
    addOverlaySlider("m_overlayHeight", "Overlay height", 180, 1000, 520, " px");
    addOverlaySlider("m_overlayTextSize", "Overlay text size", 8, 24, 12, " px");
    addOverlaySlider("m_overlayPage", "Log page (newest first)", 0, 4096, 0, "");

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
    if (key == "m_startFreshLogSession") {
        if (requested) startFreshLogSession();
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
            const float bounded = std::clamp(parsed, 5.0f, 300.0f);
            {
                std::lock_guard<std::mutex> lock(m_exportMutex);
                m_autoExportIntervalSeconds = static_cast<int>(std::lround(bounded));
            }
            m_exportCondition.notify_all();
            return true;
        } catch (...) {
            return false;
        }
    }

    if (key == "m_overlayX" || key == "m_overlayY" || key == "m_overlayWidth" ||
        key == "m_overlayTextSize" || key == "m_overlayHeight" || key == "m_overlayPage") {
        try {
            std::size_t parsedLength = 0;
            const std::string text(value);
            const float parsed = std::stof(text, &parsedLength);
            if (parsedLength != text.size() || !std::isfinite(parsed)) return false;
            const int v = static_cast<int>(std::lround(parsed));
            if (key == "m_overlayX") m_overlayX = std::clamp(v, 0, 1800);
            else if (key == "m_overlayY") m_overlayY = std::clamp(v, 0, 1000);
            else if (key == "m_overlayWidth") m_overlayWidth = std::clamp(v, 240, 1400);
            else if (key == "m_overlayTextSize") m_overlayTextSize = std::clamp(v, 8, 24);
            else if (key == "m_overlayHeight") m_overlayHeight = std::clamp(v, 180, 1000);
            else m_overlayPage = std::clamp(v, 0, 4096);
            m_nextOverlayRefresh = {};
            m_cachedOverlayLines.clear();
            return true;
        } catch (...) { return false; }
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
                j.value("m_autoExportIntervalSeconds", m_autoExportIntervalSeconds), 5, 300);
        } else if (j.contains("m_autoExportIntervalMinutes")) {
            // Migrate settings from older builds, which stored the interval in minutes.
            const int oldMinutes = std::clamp(j.value("m_autoExportIntervalMinutes", 1), 1, 60);
            m_autoExportIntervalSeconds = std::clamp(oldMinutes * 60, 5, 300);
        }

        m_overlayX = std::clamp(j.value("m_overlayX", m_overlayX), 0, 1800);
        m_overlayY = std::clamp(j.value("m_overlayY", m_overlayY), 0, 1000);
        m_overlayWidth = std::clamp(j.value("m_overlayWidth", m_overlayWidth), 240, 1400);
        m_overlayTextSize = std::clamp(j.value("m_overlayTextSize", m_overlayTextSize), 8, 24);
        m_overlayHeight = std::clamp(j.value("m_overlayHeight", m_overlayHeight), 180, 1000);
        m_overlayPage = std::clamp(j.value("m_overlayPage", m_overlayPage), 0, 4096);
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
    j["m_startFreshLogSession"] = false;
    j["m_autoExportEnabled"] = m_autoExportEnabled;
    j["m_autoExportIntervalSeconds"] = m_autoExportIntervalSeconds;
    j.erase("m_autoExportIntervalMinutes");
    j["m_exportFormat"] = m_exportFormat;

    j["m_overlayX"] = m_overlayX;
    j["m_overlayY"] = m_overlayY;
    j["m_overlayWidth"] = m_overlayWidth;
    j["m_overlayTextSize"] = m_overlayTextSize;
    j["m_overlayHeight"] = m_overlayHeight;
    j["m_overlayPage"] = m_overlayPage;
}

ModuleDiagnosticsModule::~ModuleDiagnosticsModule() {
    stopExportWorker();
}

void ModuleDiagnosticsModule::onEnable() {
    // A fresh module activation starts a clean gameplay/test log. The previous
    // rolling JSON is merged into the lifetime file before the current file is
    // removed and recreated, so test sessions never overlap.
    startFreshLogSession();
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

void ModuleDiagnosticsModule::startFreshLogSession() {
    const std::filesystem::path outputDirectory("/sdcard/Download/Bedaim");
    const auto currentPath = outputDirectory / "bedaim_diagnostics_auto.json";
    const auto lifetimePath = outputDirectory / "bedaim_diagnostics_total.json";
    const auto gameplayPath = outputDirectory / "bedaim_diagnostics_gameplay.json";
    const auto gameplayLifetimePath = outputDirectory / "bedaim_diagnostics_gameplay_total.json";
    std::string archiveError;
    bool rotationOk = true;
    {
        // Serialize archive/delete against the rolling writer. Never delete either
        // current-session file unless its entries were safely merged to lifetime.
        std::lock_guard<std::mutex> fileLock(m_fileWriteMutex);
        std::error_code existsError;
        const bool hasCurrent = std::filesystem::exists(currentPath, existsError);
        if (existsError) {
            rotationOk = false;
            archiveError = "cannot inspect current-session JSON: " + existsError.message();
        } else if (hasCurrent) {
            rotationOk = mergeCurrentSessionIntoLifetime(currentPath, lifetimePath, &archiveError);
            if (rotationOk) {
                std::error_code removeError;
                std::filesystem::remove(currentPath, removeError);
                if (removeError) {
                    rotationOk = false;
                    archiveError = "could not replace current-session JSON: " + removeError.message();
                }
            }
        }
        if (rotationOk) {
            std::error_code gameplayExistsError;
            const bool hasGameplay = std::filesystem::exists(gameplayPath, gameplayExistsError);
            if (gameplayExistsError) {
                rotationOk = false;
                archiveError = "cannot inspect gameplay-only JSON: " + gameplayExistsError.message();
            } else if (hasGameplay) {
                rotationOk = mergeCurrentSessionIntoLifetime(
                    gameplayPath, gameplayLifetimePath, &archiveError);
                if (rotationOk) {
                    std::error_code removeGameplayError;
                    std::filesystem::remove(gameplayPath, removeGameplayError);
                    if (removeGameplayError) {
                        rotationOk = false;
                        archiveError = "could not rotate gameplay-only JSON: " + removeGameplayError.message();
                    }
                }
            }
        }
        std::error_code staleError;
        std::filesystem::remove(outputDirectory / "bedaim_diagnostics_auto.txt", staleError);
        // Session rotation already archives the previous current-session files.
        // Let the new current-session snapshots accumulate for a minute before
        // rewriting the cumulative archives again.
        if (rotationOk) {
            m_nextLifetimeMerge = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        }
    }
    if (!rotationOk) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error,
            "Diagnostics", "session rotation aborted to protect previous logs: " + archiveError);
        std::lock_guard<std::mutex> lock(m_exportMutex);
        m_lastExportStatus = "Session rotation aborted; previous logs preserved";
        return;
    }
    // Keep only stable startup facts that are essential for diagnosing native
    // routes (especially AutoHit's attack hook and AutoTotem's offhand reader).
    // Old gameplay decisions/toggles are discarded so the new export is clean.
    const auto previousEntries = bedrocktools::diagnostics::snapshot();
    std::vector<bedrocktools::diagnostics::Entry> baselineEntries;
    baselineEntries.reserve(previousEntries.size());
    for (const auto& entry : previousEntries) {
        const bool keep =
            entry.component == "Runtime" ||
            entry.component == "GameHooks" ||
            entry.component == "InventoryAccess" ||
            entry.component == "Config" ||
            (entry.component == "ModuleRegistry" &&
                entry.message.rfind("registered modules initialized:", 0) == 0) ||
            ((entry.component == "AutoHit" || entry.component == "AutoTotem") &&
                entry.message.rfind("lifecycle=initialized", 0) == 0) ||
            ((entry.component == "BedrockAim" || entry.component == "HitboxExpander") &&
                entry.message.rfind("native_dependencies=ready", 0) == 0);
        if (keep) baselineEntries.push_back(entry);
    }

    bedrocktools::diagnostics::clear();
    for (const auto& entry : baselineEntries) {
        std::string message = entry.message;
        constexpr std::string_view baselinePrefix = "baseline: ";
        if (message.rfind(baselinePrefix, 0) == 0)
            message.erase(0, baselinePrefix.size());
        bedrocktools::diagnostics::log(entry.level, entry.component,
            std::string(baselinePrefix) + message);
    }
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,
        "Diagnostics", "fresh gameplay log session started; current-session JSON reset");
    if (!archiveError.empty()) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning,
            "Diagnostics", "lifetime/current log rotation warning: " + archiveError);
    }

    // The rolling current-session JSON is rewritten from this session only.
    // Timestamped manual exports remain available as historical captures.
    const bool saved = exportLogs(true);
    if (saved) {
        std::lock_guard<std::mutex> lock(m_exportMutex);
        m_lastExportStatus = "Fresh session saved (" + localClockLabel() + ")";
    }
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
    // Manual, periodic and fresh-session exports share the same rolling file.
    // Serialize the snapshot/write pair so an older snapshot cannot overwrite
    // a freshly reset session after it has been exported.
    std::lock_guard<std::mutex> fileLock(m_fileWriteMutex);
    std::string format;
    {
        std::lock_guard<std::mutex> lock(m_exportMutex);
        // The two rolling analysis files are always JSON. The format selector
        // applies only to timestamped manual exports.
        format = automatic ? "json" : m_exportFormat;
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
    const auto gameplayPath = (outputDirectory / "bedaim_diagnostics_gameplay.json").string();
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Diagnostics",
        std::string(automatic ? "periodic" : "manual") + " log export requested; format=" + format);
    std::string error;
    const bool ok = format == "json"
        ? bedrocktools::diagnostics::exportJsonTo(outputPath, &error)
        : bedrocktools::diagnostics::exportTo(outputPath, &error);
    // Both analysis modes are emitted by default: the full stream for startup
    // and native-hook diagnosis, plus a gameplay-focused stream for module gates.
    std::string gameplayError;
    const bool gameplayOk = automatic
        ? exportGameplayOnlyJsonTo(gameplayPath, &gameplayError)
        : true;

    // Cumulative histories are intentionally merged less often than the rolling
    // current-session snapshots. Parsing and rewriting a growing multi-megabyte
    // JSON archive every second burns CPU and storage bandwidth on mobile devices.
    // Session rotation archives the previous session; live history catches up
    // at least once per minute while the two rolling files keep their own cadence.
    const auto exportNow = std::chrono::steady_clock::now();
    const bool lifetimeSyncDue = automatic &&
        (m_nextLifetimeMerge.time_since_epoch().count() == 0 ||
         exportNow >= m_nextLifetimeMerge);
    std::string lifetimeError;
    bool lifetimeOk = true;
    std::string gameplayLifetimeError;
    bool gameplayLifetimeOk = true;
    if (lifetimeSyncDue) {
        lifetimeOk = ok && mergeCurrentSessionIntoLifetime(
            outputDirectory / "bedaim_diagnostics_auto.json",
            outputDirectory / "bedaim_diagnostics_total.json", &lifetimeError);
        gameplayLifetimeOk = gameplayOk && mergeCurrentSessionIntoLifetime(
            gameplayPath, outputDirectory / "bedaim_diagnostics_gameplay_total.json",
            &gameplayLifetimeError);
        // Throttle retries too: a corrupt/inaccessible archive should produce a
        // visible error and retry on the next scheduled sync, not on every snapshot.
        m_nextLifetimeMerge = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    }
    if (ok && automatic) {
        if (!gameplayOk) {
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error,
                "Diagnostics", "GAMEPLAY_ONLY_LOGS export failed: " + gameplayError);
        } else if (lifetimeSyncDue && !gameplayLifetimeOk) {
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error,
                "Diagnostics", "GAMEPLAY_ONLY_LOGS lifetime update failed: " + gameplayLifetimeError);
        }
        std::error_code staleError;
        std::filesystem::remove(outputDirectory / "bedaim_diagnostics_auto.txt", staleError);
        if (staleError) {
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning,
                "Diagnostics", "could not remove stale rolling text export: " + staleError.message());
        }
        if (lifetimeSyncDue && !lifetimeOk) {
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error,
                "Diagnostics", "lifetime log update failed: " + lifetimeError);
        }
    }

    bedrocktools::diagnostics::log(ok ? bedrocktools::diagnostics::Level::Info :
        bedrocktools::diagnostics::Level::Error, "Diagnostics",
        ok ? "diagnostic logs exported to " + outputPath :
             "diagnostic log export failed: " + error);

    {
        std::lock_guard<std::mutex> lock(m_exportMutex);
        const std::string completedAt = localClockLabel();
        if (ok) {
            if (!automatic) {
                m_lastExportStatus = "Manual export completed (" + completedAt + ")";
            } else if (!gameplayOk || (lifetimeSyncDue && (!lifetimeOk || !gameplayLifetimeOk))) {
                m_lastExportStatus = "Some diagnostic outputs failed (" + completedAt + ")";
            } else if (lifetimeSyncDue) {
                m_lastExportStatus = "Rolling logs + lifetime histories synced (" + completedAt + ")";
            } else {
                const auto secondsRemaining = std::max<std::int64_t>(0,
                    std::chrono::duration_cast<std::chrono::seconds>(
                        m_nextLifetimeMerge - std::chrono::steady_clock::now()).count());
                m_lastExportStatus = "Rolling logs saved; history sync in " +
                    std::to_string(secondsRemaining) + "s (" + completedAt + ")";
            }
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
    const float originX = static_cast<float>(m_overlayX);
    const float originY = static_cast<float>(m_overlayY);
    const float overlayWidth = static_cast<float>(m_overlayWidth);
    const float textSize = static_cast<float>(m_overlayTextSize);
    const float overlayHeight = static_cast<float>(m_overlayHeight);
    const float lineHeight = textSize + 4.0f;
    const std::string line1 = "Module Diagnostics: " + std::to_string(m_summary.total) +
        " modules | " + std::to_string(m_summary.enabled) + " enabled";
    const std::string line2 = "Logs " + std::to_string(bedrocktools::diagnostics::count()) +
        "/" + std::to_string(bedrocktools::diagnostics::bufferCapacity()) +
        " | lost " + std::to_string(bedrocktools::diagnostics::droppedCount()) +
        " | Reg W " + std::to_string(m_summary.warnings) +
        " E " + std::to_string(m_summary.errors);
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
        command.x = originX;
        command.y = originY + y;
        command.w = overlayWidth;
        command.h = lineHeight;
        command.size = textSize;
        command.color = color;
        command.text = text.c_str();
        commands.push_back(command);
    };

    addText(line1, 0.0f, 0xFFFFFFFF);
    addText(line2, lineHeight, (m_summary.errors || m_summary.warnings) ? 0xFFFFC060 : 0xFF80FF80);
    addText(line3, lineHeight * 2.0f, m_summary.written ? 0xFF80FF80 : 0xFFFF7070);
    addText(line4, lineHeight * 3.0f, exportStatus.find("failed") != std::string::npos ? 0xFFFF7070 : 0xFFFFFFFF);
    addText(line5, lineHeight * 4.0f, 0xFFFFFFFF);

    // Rebuild the visible log page at most twice per second. The previous renderer
    // copied the full 32k-entry ring and allocated strings for every wrapped line
    // on every frame; at hundreds/thousands of entries this can dominate mobile FPS.
    const auto displayNow = std::chrono::steady_clock::now();
    if (m_cachedOverlayLines.empty() || displayNow >= m_nextOverlayRefresh) {
        const auto runtimeEntries = bedrocktools::diagnostics::snapshot();
        const std::size_t approxCharsPerLine = std::max<std::size_t>(12,
            static_cast<std::size_t>(overlayWidth / std::max(1.0f, textSize * 0.58f)));
        const std::size_t availableLines = std::max<std::size_t>(1,
            static_cast<std::size_t>(std::max(1.0f, (overlayHeight - lineHeight * 6.0f) / lineHeight)));

        // First count wrapped lines without allocating line strings. This is linear
        // in the current bounded ring but runs only twice per second, not each frame.
        std::size_t totalLines = 0;
        for (const auto& entry : runtimeEntries) {
            const std::size_t prefixLength = 2 + std::to_string(entry.sequence).size() +
                1 + bedrocktools::diagnostics::levelName(entry.level).size() + 1 +
                entry.component.size() + 2;
            const std::size_t chars = prefixLength + entry.message.size();
            totalLines += std::max<std::size_t>(1, (chars + approxCharsPerLine - 1) / approxCharsPerLine);
        }
        m_cachedOverlayPageCount = std::max<std::size_t>(1,
            (totalLines + availableLines - 1) / availableLines);
        const std::size_t page = std::min<std::size_t>(
            static_cast<std::size_t>(std::max(0, m_overlayPage)), m_cachedOverlayPageCount - 1);
        const std::size_t firstLine = page * availableLines;
        const std::size_t lastLine = std::min(totalLines, firstLine + availableLines);

        m_cachedOverlayLines.clear();
        m_cachedOverlayLines.reserve(availableLines);
        std::size_t lineIndex = 0;
        for (const auto& entry : runtimeEntries) {
            const std::string tag = entry.level == bedrocktools::diagnostics::Level::Warning ? "WARN" :
                entry.level == bedrocktools::diagnostics::Level::Error ? "ERR" :
                entry.level == bedrocktools::diagnostics::Level::Critical ? "CRIT" :
                bedrocktools::diagnostics::levelName(entry.level);
            const std::string fullMessage = "[" + std::to_string(entry.sequence) + "] " + tag + " " +
                entry.component + ": " + entry.message;
            const std::uint32_t color =
                entry.level == bedrocktools::diagnostics::Level::Critical ||
                entry.level == bedrocktools::diagnostics::Level::Error ? 0xFFFF7070 :
                entry.level == bedrocktools::diagnostics::Level::Warning ? 0xFFFFC060 : 0xFFE8E8E8;
            const std::size_t wrappedCount = std::max<std::size_t>(1,
                (fullMessage.size() + approxCharsPerLine - 1) / approxCharsPerLine);
            for (std::size_t line = 0; line < wrappedCount; ++line, ++lineIndex) {
                if (lineIndex < firstLine || lineIndex >= lastLine) continue;
                const std::size_t offset = line * approxCharsPerLine;
                m_cachedOverlayLines.emplace_back(
                    fullMessage.substr(offset, approxCharsPerLine), color);
            }
        }
        m_nextOverlayRefresh = displayNow + std::chrono::milliseconds(500);
    }

    const std::size_t pageCount = std::max<std::size_t>(1, m_cachedOverlayPageCount);
    const std::size_t page = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(0, m_overlayPage)), pageCount - 1);
    addText("Log page " + std::to_string(page + 1) + "/" + std::to_string(pageCount) +
            " (oldest to newest; change Log page in settings)", lineHeight * 5.0f, 0xFFB0D8FF);
    float y = lineHeight * 6.0f;
    for (const auto& line : m_cachedOverlayLines) {
        addText(line.first, y, line.second);
        y += lineHeight;
    }
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
        {"capacity", bedrocktools::diagnostics::bufferCapacity()},
        {"dropped_entries", bedrocktools::diagnostics::droppedCount()},
        {"recent_entries", json::array()}
    };
    for (std::size_t i = firstRuntimeEvent; i < runtimeEvents.size(); ++i) {
        const auto& entry = runtimeEvents[i];
        report["runtime_log"]["recent_entries"].push_back({
            {"timestamp_ms", entry.timestampMs},
            {"monotonic_ns", entry.monotonicNs},
            {"thread_id", entry.threadId},
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
