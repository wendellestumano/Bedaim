#pragma once

#include "../Module.hpp"
#include <chrono>
#include <string>
#include <string_view>

class ConfigurationsModule final : public Module {
public:
    ConfigurationsModule();
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    std::string outputPath() const;
    nlohmann::json currentSnapshot() const;
    void saveAll();

    int intervalSeconds = 5;
    std::string fileName = "Configs.json";
    bool changeDetection = false;
    std::chrono::steady_clock::time_point m_nextSave{};
    nlohmann::json m_lastSnapshot;
    bool m_hasSnapshot = false;
};
