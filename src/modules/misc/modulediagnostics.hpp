#pragma once

#include "../Module.hpp"
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>
#include <string_view>

class ModuleDiagnosticsModule final : public Module {
public:
    ModuleDiagnosticsModule();
    ~ModuleDiagnosticsModule() override;

    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    struct ReportSummary {
        std::size_t total = 0;
        std::size_t enabled = 0;
        std::size_t warnings = 0;
        std::size_t errors = 0;
        bool written = false;
        std::string outputPath;
    };

    ReportSummary runDiagnostics();
    bool exportLogs(bool automatic);
    void startFreshLogSession();
    void startExportWorker();
    void stopExportWorker();
    void exportWorkerLoop();
    std::chrono::steady_clock::time_point m_nextRefresh{};
    ReportSummary m_summary{};
    bool m_reportReady = false;
    bool m_exportLogsNow = false;
    bool m_clearLogsNow = false;
    bool m_autoExportEnabled = true;
    bool m_logAllEnabled = true;
    bool m_gameplayOnlyLogsEnabled = true;
    int m_autoExportIntervalSeconds = 5;
    std::string m_exportFormat = "txt";
    mutable std::mutex m_exportMutex;
    std::mutex m_fileWriteMutex;
    std::condition_variable m_exportCondition;
    std::thread m_exportWorker;
    bool m_stopExportWorker = true;
    std::string m_lastExportStatus = "No export attempted yet";
    std::string m_lastExportPath;
    std::string m_lastSuccessfulExportTime = "never";
};
