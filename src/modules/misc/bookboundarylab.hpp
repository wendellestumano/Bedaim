#pragma once

#include "../Module.hpp"
#include <string>
#include <string_view>

class BookBoundaryLabModule final : public Module {
public:
    BookBoundaryLabModule();

    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    void buildPagePlan() const;

    std::string m_sourceText;
    int m_codePointsPerPage = 240;
    int m_maxPages = 200;
};
