#pragma once

#include "BookPaginator.hpp"
#include "../Module.hpp"

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class BookBoundaryLabModule final : public Module {
public:
    BookBoundaryLabModule();

    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    void buildPagePlan();

    mutable std::mutex m_stateMutex;
    std::string m_sourceText;
    std::vector<bedaim::books::Page> m_pages;
    std::string m_planError;
    std::size_t m_currentPageIndex = 0;
    int m_codePointsPerPage = 240;
    int m_maxPages = bedaim::books::kMaxPlannedPages;
};
