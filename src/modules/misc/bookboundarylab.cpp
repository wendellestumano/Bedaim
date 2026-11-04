#include "bookboundarylab.hpp"

#include "BookPaginator.hpp"
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
}

BookBoundaryLabModule::BookBoundaryLabModule()
    : Module("Book Expander",
             "Locally splits longer notes into UTF-8 code-point-safe page-sized chunks. It does not change Minecraft validation or send book data.",
             "bookboundarylab") {
    keybind = 0;
}

void BookBoundaryLabModule::onMenuRegistered() {
    using namespace pl::modmenu;

    ConfigSchemaBuilder schema;
    schema.defaultCategory("book_expander")
        .category("book_expander", "Book Expander",
                  "Prepare longer notes as a sequence of bounded pages. Page text is written to the local diagnostics log for manual copying; nothing is sent.");

    ConfigNodeV2 source;
    source.id = "m_sourceText";
    source.key = "m_sourceText";
    source.title = "Text to paginate";
    source.description = "Paste coordinates, poems, notes or other text (up to 8 KiB). This field is kept in memory only and is not saved in BedAim config.";
    source.category = "book_expander";
    source.type = ConfigControlTypeV2::Text;
    source.defaultValue = "";
    source.currentValue = m_sourceText;
    source.hasCurrentValue = true;
    source.placeholder = "Paste your book text here...";
    source.maxLength = 8192;
    schema.node(std::move(source));

    ConfigNodeV2 perPage;
    perPage.id = "m_codePointsPerPage";
    perPage.key = "m_codePointsPerPage";
    perPage.title = "Planning budget per page";
    perPage.description = "Maximum Unicode code points in each local page chunk (32–256). This is a planning setting, not a claim about the engine's exact internal limit.";
    perPage.category = "book_expander";
    perPage.type = ConfigControlTypeV2::SliderInt;
    perPage.minValue = "32";
    perPage.maxValue = "256";
    perPage.step = "1";
    perPage.defaultValue = "240";
    perPage.currentValue = std::to_string(m_codePointsPerPage);
    perPage.hasCurrentValue = true;
    schema.node(std::move(perPage));

    ConfigNodeV2 maxPages;
    maxPages.id = "m_maxPages";
    maxPages.key = "m_maxPages";
    maxPages.title = "Maximum planned pages";
    maxPages.description = "Local safety cap for the page planner (1–100).";
    maxPages.category = "book_expander";
    maxPages.type = ConfigControlTypeV2::SliderInt;
    maxPages.minValue = "1";
    maxPages.maxValue = "100";
    maxPages.step = "1";
    maxPages.defaultValue = "50";
    maxPages.currentValue = std::to_string(m_maxPages);
    maxPages.hasCurrentValue = true;
    schema.node(std::move(maxPages));

    ConfigNodeV2 build;
    build.id = "m_buildPagePlan";
    build.key = "m_buildPagePlan";
    build.title = "Build local page plan";
    build.description = "Splits the text, preserves Unicode and logs each page locally for manual copying. No inventory, book item or network operation.";
    build.category = "book_expander";
    build.type = ConfigControlTypeV2::Button;
    build.actionValue = "build";
    schema.node(std::move(build));

    setConfigSchemaJson(moduleId, schema.toJson());
}

bool BookBoundaryLabModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "m_sourceText") {
        if (value.size() > 8192) return false;
        m_sourceText.assign(value);
        return true;
    }
    if (key == "m_codePointsPerPage") {
        return parseBoundedInteger(value, 32, 256, m_codePointsPerPage);
    }
    if (key == "m_maxPages") {
        return parseBoundedInteger(value, 1, 100, m_maxPages);
    }
    if (key == "m_buildPagePlan") {
        buildPagePlan();
        return true;
    }
    return false;
}

void BookBoundaryLabModule::buildPagePlan() const {
    const auto plan = bedaim::books::paginate(
        m_sourceText,
        static_cast<std::size_t>(m_codePointsPerPage),
        static_cast<std::size_t>(m_maxPages));

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
        ". Page content follows in local diagnostics; no book or network payload was created.";
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
    m_codePointsPerPage = std::clamp(j.value("m_codePointsPerPage", m_codePointsPerPage), 32, 256);
    m_maxPages = std::clamp(j.value("m_maxPages", j.value("m_targetPages", m_maxPages)), 1, 100);
    // Deliberately do not restore source text from config: notes may contain private coordinates.
    m_sourceText.clear();
}

void BookBoundaryLabModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_codePointsPerPage"] = m_codePointsPerPage;
    j["m_maxPages"] = m_maxPages;
}