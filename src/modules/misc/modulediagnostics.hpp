#pragma once

#include "../Module.hpp"
#include <chrono>
#include <string>
#include <string_view>

class ModuleDiagnosticsModule final : public Module {
public:
    ModuleDiagnosticsModule();

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
    std::chrono::steady_clock::time_point m_nextRefresh{};
    ReportSummary m_summary{};
    bool m_reportReady = false;
    bool m_exportLogsNow = false;
    bool m_clearLogsNow = false;
};
