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
               currentPayload.compare(currentPayload.size() - 2, 2, "\\\\n") == 0) {
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
                   lifetimePayload.compare(lifetimePayload.size() - 2, 2, "\\\\n") == 0) {
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

        auto entryKey = [](const json& entry) {
            return std::to_string(entry.value("timestamp_ms", std::int64_t{0})) + "|" +
                std::to_string(entry.value("sequence", std::uint64_t{0})) + "|" +
                entry.value("level", std::string{}) + "|" +
                entry.value("component", std::string{}) + "|" +
                entry.value("message", std::string{});
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

        lifetime["schema_version"] = 1;
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
        payload["schema_version"] = 1;
        payload["mode"] = "GAMEPLAY_ONLY_LOGS";
        payload["generated_at_unix_ms"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        payload["buffer_capacity"] = 4096;
        payload["entries"] = json::array();
        for (const auto& entry : entries) {
            if (!isGameplay(entry)) continue;
            payload["entries"].push_back({
                {"timestamp_ms", entry.timestampMs},
                {"sequence", entry.sequence},
                {"level", bedrocktools::diagnostics::levelName(entry.level)},
                {"component", entry.component},
                {"message", entry.message}
            });
        }
        payload["entry_count"] = payload["entries"].size();
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
    autoNode.description = "While enabled, refreshes bedaim_diagnostics_auto.json for the current session and merges new entries into bedaim_diagnostics_total.json across all sessions.";
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
            const float bounded = std::clamp(parsed, 1.0f, 300.0f);
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
    j["m_startFreshLogSession"] = false;
    j["m_autoExportEnabled"] = m_autoExportEnabled;
    j["m_autoExportIntervalSeconds"] = m_autoExportIntervalSeconds;
    j.erase("m_autoExportIntervalMinutes");
    j["m_exportFormat"] = m_exportFormat;
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

    // Maintain a separate lifetime JSON containing deduplicated entries from
    // every gameplay. Merge from the just-written current snapshot, not from
    // the mutable in-memory buffer, so repeated five-second saves are idempotent.
    std::string lifetimeError;
    const bool lifetimeOk = ok && automatic && mergeCurrentSessionIntoLifetime(
        outputDirectory / "bedaim_diagnostics_auto.json",
        outputDirectory / "bedaim_diagnostics_total.json", &lifetimeError);
    std::string gameplayLifetimeError;
    const bool gameplayLifetimeOk = gameplayOk && automatic && mergeCurrentSessionIntoLifetime(
        gameplayPath, outputDirectory / "bedaim_diagnostics_gameplay_total.json",
        &gameplayLifetimeError);
    if (ok && automatic) {
        if (!gameplayOk) {
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error,
                "Diagnostics", "GAMEPLAY_ONLY_LOGS export failed: " + gameplayError);
        } else if (!gameplayLifetimeOk) {
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error,
                "Diagnostics", "GAMEPLAY_ONLY_LOGS lifetime update failed: " + gameplayLifetimeError);
        }
        std::error_code staleError;
        std::filesystem::remove(outputDirectory / "bedaim_diagnostics_auto.txt", staleError);
        if (staleError) {
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning,
                "Diagnostics", "could not remove stale rolling text export: " + staleError.message());
        }
        if (!lifetimeOk) {
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
            m_lastExportStatus = automatic
                ? (lifetimeOk && gameplayOk && gameplayLifetimeOk
                    ? "ALL + GAMEPLAY_ONLY_LOGS + both histories saved (" + completedAt + ")"
                    : "Some diagnostic outputs failed (" + completedAt + ")")
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

    // Show actual runtime warnings/errors, not just registry consistency counts.
    // Scan newest-first, then display chronologically so a failure gate is visible
    // in the overlay without opening the JSON export.
    const auto runtimeEntries = bedrocktools::diagnostics::snapshot();
    std::vector<std::string> recentAlerts;
    recentAlerts.reserve(4);
    for (auto it = runtimeEntries.rbegin(); it != runtimeEntries.rend() && recentAlerts.size() < 4; ++it) {
        if (it->level != bedrocktools::diagnostics::Level::Warning &&
            it->level != bedrocktools::diagnostics::Level::Error &&
            it->level != bedrocktools::diagnostics::Level::Critical) continue;
        const char* tag = it->level == bedrocktools::diagnostics::Level::Warning ? "WARN" :
            it->level == bedrocktools::diagnostics::Level::Critical ? "CRIT" : "ERR";
        std::string message = std::string("[") + tag + "] " + it->component + ": " + it->message;
        constexpr std::size_t maxOverlayChars = 76;
        if (message.size() > maxOverlayChars) message = message.substr(0, maxOverlayChars - 3) + "...";
        recentAlerts.push_back(std::move(message));
    }
    std::reverse(recentAlerts.begin(), recentAlerts.end());
    for (std::size_t i = 0; i < recentAlerts.size(); ++i) {
        addText(recentAlerts[i], 92.0f + static_cast<float>(i) * 16.0f, 0xFFFF7070);
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
