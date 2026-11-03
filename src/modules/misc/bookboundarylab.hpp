#pragma once

#include "../Module.hpp"
#include <cstdint>
#include <string_view>

class BookBoundaryLabModule final : public Module {
public:
    BookBoundaryLabModule();

    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    void logEstimate() const;

    int m_targetPages = 67;
    int m_codePointsPerPage = 1000;
};
