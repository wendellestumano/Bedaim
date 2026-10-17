#pragma once

#include "../Module.hpp"
#include <chrono>
#include <string>

class ModuleDiagnosticsModule final : public Module {
public:
    ModuleDiagnosticsModule();

    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

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
};
