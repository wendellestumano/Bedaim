#pragma once

#include "../Module.hpp"
#include <string_view>

class DiagnosticsModule final : public Module {
public:
    DiagnosticsModule();
    void onInit() override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    bool m_exportLogsNow = false;
    bool m_clearLogsNow = false;
};
