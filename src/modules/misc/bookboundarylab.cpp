#include "bookboundarylab.hpp"

#include "../../core/Diagnostics.hpp"
#include "../../config/AtomicConfigWriter.hpp"
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace {
constexpr const char* kBookSourcePath = "/sdcard/Download/Bedaim/book_input.txt";
constexpr const char* kBookPlanExportPath = "/sdcard/Download/Bedaim/book_pages.txt";

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
    std::string sourceText;
    std::string previewTitle = "Local page preview";
    std::string previewDescription =
        "No local page plan yet. Enter text, build the plan, then use Previous/Next to inspect pages.";
    std::size_t current = 0;
    std::size_t total = 0;

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        codePointsPerPage = m_codePointsPerPage;
        maxPages = m_maxPages;
        sourceText = m_sourceText;
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
    source.currentValue = sourceText;
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

    ConfigNodeV2 loadFile;
    loadFile.id = "m_loadTextFile";
    loadFile.key = "m_loadTextFile";
    loadFile.title = "Load text from Downloads";
    loadFile.description = std::string("Loads up to 64 KiB from ") + kBookSourcePath +
        ". Use this instead of opening the Android keyboard over the native book screen.";
    loadFile.category = "book_expander";
    loadFile.type = ConfigControlTypeV2::Button;
    loadFile.actionValue = "load";
    schema.node(std::move(loadFile));

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

    ConfigNodeV2 exportPlan;
    exportPlan.id = "m_exportPagePlan";
    exportPlan.key = "m_exportPagePlan";
    exportPlan.title = "Save all planned pages to Downloads";
    exportPlan.description = std::string("Writes the complete local page plan to ") +
        kBookPlanExportPath + ". This is a text export, not a Minecraft book item.";
    exportPlan.category = "book_expander";
    exportPlan.type = ConfigControlTypeV2::Button;
    exportPlan.actionValue = "export";
    schema.node(std::move(exportPlan));

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

    if (key == "m_loadTextFile") {
        const std::filesystem::path sourcePath(kBookSourcePath);
        std::error_code ec;
        std::filesystem::create_directories(sourcePath.parent_path(), ec);
        if (ec) {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Error, moduleId,
                "Cannot create Download/Bedaim: " + ec.message());
            return true;
        }

        if (!std::filesystem::exists(sourcePath, ec) || ec) {
            std::ofstream createFile(sourcePath, std::ios::binary);
            if (!createFile) {
                bedrocktools::diagnostics::log(
                    bedrocktools::diagnostics::Level::Error, moduleId,
                    std::string("Cannot create source file: ") + kBookSourcePath);
            } else {
                bedrocktools::diagnostics::log(
                    bedrocktools::diagnostics::Level::Warning, moduleId,
                    std::string("Created empty source file: ") + kBookSourcePath +
                    ". Paste your text into it, then press Load text from Downloads again.");
            }
            return true;
        }

        const auto fileSize = std::filesystem::file_size(sourcePath, ec);
        if (ec || fileSize > 65536) {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Warning, moduleId,
                "Source file rejected: it must be readable and no larger than 64 KiB.");
            return true;
        }

        std::ifstream input(sourcePath, std::ios::binary);
        if (!input) {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Error, moduleId,
                std::string("Cannot read source file: ") + kBookSourcePath +
                ". Check Android storage access.");
            return true;
        }
        std::string fileText((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
        if (fileText.size() > 65536) {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Warning, moduleId,
                "Source file grew beyond the 64 KiB cap while being read.");
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_sourceText = std::move(fileText);
        }
        bedrocktools::diagnostics::log(
            bedrocktools::diagnostics::Level::Info, moduleId,
            std::string("Loaded source file: ") + kBookSourcePath +
            ". Build the local page plan to inspect its pages.");
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

    if (key == "m_exportPagePlan") {
        std::vector<bedaim::books::Page> pages;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            pages = m_pages;
        }
        if (pages.empty()) {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Warning, moduleId,
                "No page plan to export; build a local page plan first.");
            return true;
        }

        const std::filesystem::path outputPath(kBookPlanExportPath);
        std::error_code ec;
        std::filesystem::create_directories(outputPath.parent_path(), ec);
        if (ec) {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Error, moduleId,
                "Cannot create Download/Bedaim for page export: " + ec.message());
            return true;
        }

        std::string payload;
        payload.reserve(256 + 65536 + pages.size() * 96);
        payload += "# Bedaim Book Expander - local page export\n";
        payload += "# pages=" + std::to_string(pages.size()) + "\n";
        payload += "# This file is not a native Minecraft book. Copy page text manually.\n";
        for (std::size_t i = 0; i < pages.size(); ++i) {
            const auto& page = pages[i];
            payload += "\n===== PAGE " + std::to_string(i + 1) + "/" +
                std::to_string(pages.size()) + " | CODE_POINTS=" +
                std::to_string(page.codePoints) + " | UTF8_BYTES=" +
                std::to_string(page.utf8Bytes) + " | UTF16_UNITS=" +
                std::to_string(page.utf16Units) + " =====\n";
            payload += page.text;
            if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
        }

        if (!bedrocktools::config::writePayloadAtomically(outputPath.string(), payload)) {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Error, moduleId,
                std::string("Could not write page export: ") + kBookPlanExportPath);
        } else {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Info, moduleId,
                "Saved " + std::to_string(pages.size()) + " planned pages to " +
                kBookPlanExportPath + ". Export is local text, not a native book.");
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

    // Do not copy every page body into diagnostics or rolling auto-export logs.
    // The user can explicitly save the entire plan to book_pages.txt or log one page.
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
