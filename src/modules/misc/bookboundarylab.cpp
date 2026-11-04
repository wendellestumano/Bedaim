#include "bookboundarylab.hpp"

#include "../../core/Diagnostics.hpp"
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace {
bool parseBoundedInteger(std::string_view value, int minimum, int maximum, int& output) {
    try {
        const std::string text(value);
        std::size_t used = 0;
        const float parsed = std::stof(text, &used);
        if (used != text.size() || !std::isfinite(parsed)) return false;
        const float bounded = std::clamp(parsed, static_cast<float>(minimum), static_cast<float>(maximum));
        output = static_cast<int>(std::lround(bounded));
        return true;
    } catch (...) {
        return false;
    }
}

std::string buttonLabel(const char* label, std::size_t current, std::size_t total) {
    return std::string(label) + " (page " + std::to_string(current) +
        "/" + std::to_string(total) + ")";
}
}

BookBoundaryLabModule::BookBoundaryLabModule()
    : Module("Book Expander",
             "Locally paginates longer notes into UTF-8-safe chunks. It does not modify Minecraft's native book editor or server validation.",
             "bookboundarylab") {
    keybind = 0;
}

void BookBoundaryLabModule::onMenuRegistered() {
    using namespace pl::modmenu;

    int codePointsPerPage = 240;
    int maxPages = bedaim::books::kMaxPlannedPages;
    std::string previewTitle = "Local page preview";
    std::string previewDescription =
        "No local page plan yet. Enter text, build the plan, then use Previous/Next to inspect pages.";
    std::size_t current = 0;
    std::size_t total = 0;

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        codePointsPerPage = m_codePointsPerPage;
        maxPages = m_maxPages;
        total = m_pages.size();
        if (total > 0 && m_currentPageIndex < total) {
            current = m_currentPageIndex + 1;
            const auto& page = m_pages[m_currentPageIndex];
            previewTitle = "Local page " + std::to_string(current) + "/" + std::to_string(total);
            previewDescription =
                "Code points: " + std::to_string(page.codePoints) +
                " | UTF-8 bytes: " + std::to_string(page.utf8Bytes) +
                " | UTF-16 units: " + std::to_string(page.utf16Units) +
                "\n\n" + page.text;
        } else if (!m_planError.empty()) {
            previewDescription = "Last plan failed: " + m_planError;
        }
    }

    ConfigSchemaBuilder schema;
    schema.defaultCategory("book_expander")
        .category("book_expander", "Book Expander",
                  "Prepare, inspect and manually transfer local page chunks. Native Minecraft/editor/server limits are unchanged.");

    ConfigNodeV2 source;
    source.id = "m_sourceText";
    source.key = "m_sourceText";
    source.title = "Text to paginate";
    source.description =
        "Paste up to 64 KiB here. Text remains in memory only. Recommended: finish text entry before opening the native Minecraft book; Android keyboard focus may dismiss BookScreen.";
    source.category = "book_expander";
    source.type = ConfigControlTypeV2::Text;
    source.defaultValue = "";
    source.currentValue = "";
    source.hasCurrentValue = true;
    source.placeholder = "Paste your book text here...";
    source.maxLength = 65536;
    schema.node(std::move(source));

    ConfigNodeV2 perPage;
    perPage.id = "m_codePointsPerPage";
    perPage.key = "m_codePointsPerPage";
    perPage.title = "Planning budget per page";
    perPage.description = "Maximum Unicode code points in each local page chunk (32–256). This is not a native editor limit.";
    perPage.category = "book_expander";
    perPage.type = ConfigControlTypeV2::SliderInt;
    perPage.minValue = "32";
    perPage.maxValue = "256";
    perPage.step = "1";
    perPage.defaultValue = "240";
    perPage.currentValue = std::to_string(codePointsPerPage);
    perPage.hasCurrentValue = true;
    schema.node(std::move(perPage));

    ConfigNodeV2 maxPagesNode;
    maxPagesNode.id = "m_maxPages";
    maxPagesNode.key = "m_maxPages";
    maxPagesNode.title = "Maximum planned pages";
    maxPagesNode.description = "Local page-plan cap (1–200). The Next button remains available through page 200 in this planner only.";
    maxPagesNode.category = "book_expander";
    maxPagesNode.type = ConfigControlTypeV2::SliderInt;
    maxPagesNode.minValue = "1";
    maxPagesNode.maxValue = std::to_string(bedaim::books::kMaxPlannedPages);
    maxPagesNode.step = "1";
    maxPagesNode.defaultValue = std::to_string(bedaim::books::kMaxPlannedPages);
    maxPagesNode.currentValue = std::to_string(maxPages);
    maxPagesNode.hasCurrentValue = true;
    schema.node(std::move(maxPagesNode));

    ConfigNodeV2 build;
    build.id = "m_buildPagePlan";
    build.key = "m_buildPagePlan";
    build.title = "Build local page plan";
    build.description = "Creates the local page sequence and opens page 1 in the preview below.";
    build.category = "book_expander";
    build.type = ConfigControlTypeV2::Button;
    build.actionValue = "build";
    schema.node(std::move(build));

    ConfigNodeV2 preview;
    preview.id = "m_currentPagePreview";
    preview.title = previewTitle;
    preview.description = previewDescription;
    preview.category = "book_expander";
    preview.type = ConfigControlTypeV2::Section;
    schema.node(std::move(preview));

    ConfigNodeV2 previous;
    previous.id = "m_previousPage";
    previous.key = "m_previousPage";
    previous.title = "Previous planned page";
    previous.description = "Move one page backward in the local 1–200 page plan.";
    previous.category = "book_expander";
    previous.type = ConfigControlTypeV2::Button;
    previous.actionValue = "previous";
    schema.node(std::move(previous));

    ConfigNodeV2 next;
    next.id = "m_nextPage";
    next.key = "m_nextPage";
    next.title = "Next planned page";
    next.description = "Stays available past page 50 and navigates the local plan up to page 200.";
    next.category = "book_expander";
    next.type = ConfigControlTypeV2::Button;
    next.actionValue = "next";
    schema.node(std::move(next));

    ConfigNodeV2 copy;
    copy.id = "m_copyCurrentPage";
    copy.key = "m_copyCurrentPage";
    copy.title = "Log current page for copying";
    copy.description = "Writes only the selected page chunk to local diagnostics.";
    copy.category = "book_expander";
    copy.type = ConfigControlTypeV2::Button;
    copy.actionValue = "copy";
    schema.node(std::move(copy));

    setConfigSchemaJson(moduleId, schema.toJson());
}

bool BookBoundaryLabModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "m_sourceText") {
        if (value.size() > 65536) {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Warning, moduleId,
                "Source text rejected: exceeds the 64 KiB local input cap.");
            return true;
        }
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_sourceText.assign(value);
        return true;
    }

    if (key == "m_codePointsPerPage") {
        int parsed = 0;
        if (!parseBoundedInteger(value, 32, 256, parsed)) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_codePointsPerPage = parsed;
        return true;
    }

    if (key == "m_maxPages") {
        int parsed = 0;
        if (!bedaim::books::parsePageCountSetting(value, parsed)) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_maxPages = parsed;
        return true;
    }

    if (key == "m_buildPagePlan") {
        buildPagePlan();
        return true;
    }

    if (key == "m_previousPage" || key == "m_nextPage") {
        bool moved = false;
        std::size_t selected = 0;
        std::size_t total = 0;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            total = m_pages.size();
            moved = bedaim::books::movePageCursor(
                m_currentPageIndex, total, key == "m_nextPage" ? 1 : -1);
            if (total > 0 && m_currentPageIndex < total) selected = m_currentPageIndex + 1;
        }
        if (moved) {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Info, moduleId,
                "Selected local page " + std::to_string(selected) + "/" + std::to_string(total) +
                ". This is planner navigation; the native BookScreen page index is unchanged.");
        } else {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Debug, moduleId,
                total == 0 ? "Page navigation ignored: build a local page plan first."
                           : "Page navigation reached the end of the local plan; button remains available.");
        }
        return true;
    }

    if (key == "m_copyCurrentPage") {
        std::string pageText;
        std::size_t selected = 0;
        std::size_t total = 0;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            total = m_pages.size();
            if (total > 0 && m_currentPageIndex < total) {
                selected = m_currentPageIndex + 1;
                pageText = m_pages[m_currentPageIndex].text;
            }
        }
        if (selected == 0) {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Warning, moduleId,
                "No current page to export; build a local page plan first.");
        } else {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Info, moduleId,
                "CURRENT PAGE " + std::to_string(selected) + "/" + std::to_string(total) + ": " + pageText);
        }
        return true;
    }

    return false;
}

void BookBoundaryLabModule::buildPagePlan() {
    std::string source;
    int codePointsPerPage = 240;
    int maxPages = bedaim::books::kMaxPlannedPages;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        source = m_sourceText;
        codePointsPerPage = m_codePointsPerPage;
        maxPages = m_maxPages;
    }

    const auto plan = bedaim::books::paginate(
        source,
        static_cast<std::size_t>(codePointsPerPage),
        static_cast<std::size_t>(maxPages));

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_pages = plan.ok ? plan.pages : std::vector<bedaim::books::Page>{};
        m_currentPageIndex = 0;
        m_planError = plan.ok ? std::string{} : plan.error;
    }

    if (!plan.ok) {
        bedrocktools::diagnostics::log(
            bedrocktools::diagnostics::Level::Warning, moduleId,
            "Page plan rejected: " + plan.error);
        return;
    }

    const std::string summary =
        "Local page plan ready: pages=" + std::to_string(plan.pages.size()) +
        ", code_points=" + std::to_string(plan.totalCodePoints) +
        ", utf8_bytes=" + std::to_string(plan.totalUtf8Bytes) +
        ", utf16_units=" + std::to_string(plan.totalUtf16Units) +
        ", page_limit=" + std::to_string(maxPages) +
        ". Content is stored in this module's memory only; no book or network payload was created.";
    bedrocktools::diagnostics::log(
        bedrocktools::diagnostics::Level::Info, moduleId, summary);

    for (std::size_t i = 0; i < plan.pages.size(); ++i) {
        const auto& page = plan.pages[i];
        const std::string message =
            "PAGE " + std::to_string(i + 1) + "/" + std::to_string(plan.pages.size()) +
            " [code_points=" + std::to_string(page.codePoints) +
            ", utf8_bytes=" + std::to_string(page.utf8Bytes) +
            ", utf16_units=" + std::to_string(page.utf16Units) +
            "]: " + page.text;
        bedrocktools::diagnostics::log(
            bedrocktools::diagnostics::Level::Info, moduleId, message);
    }
}

void BookBoundaryLabModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_codePointsPerPage = std::clamp(j.value("m_codePointsPerPage", m_codePointsPerPage), 32, 256);
    m_maxPages = std::clamp(
        j.value("m_maxPages", j.value("m_targetPages", m_maxPages)),
        1, bedaim::books::kMaxPlannedPages);
    // Text and page contents are deliberately ephemeral; do not write private
    // notes or the generated 200-page plan into the persistent Bedaim config.
    m_sourceText.clear();
    m_pages.clear();
    m_currentPageIndex = 0;
    m_planError.clear();
}

void BookBoundaryLabModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_codePointsPerPage"] = m_codePointsPerPage;
    j["m_maxPages"] = m_maxPages;
}
