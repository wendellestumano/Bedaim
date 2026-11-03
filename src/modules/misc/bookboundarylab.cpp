#include "bookboundarylab.hpp"

#include "../../core/Diagnostics.hpp"
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace {
bool parseBoundedInteger(std::string_view value, int minimum, int maximum, int& output) {
    try {
        const std::string text(value);
        std::size_t used = 0;
        const float parsed = std::stof(text, &used);
        if (used != text.size() || !std::isfinite(parsed)) return false;
        output = std::clamp(static_cast<int>(std::lround(parsed)), minimum, maximum);
        return true;
    } catch (...) {
        return false;
    }
}
}

BookBoundaryLabModule::BookBoundaryLabModule()
    : Module("Book Boundary Lab",
             "Local-only book limit estimator. Measures requested page and Unicode code-point counts; it does not create or send book payloads.",
             "bookboundarylab") {
    keybind = 0;
}

void BookBoundaryLabModule::onMenuRegistered() {
    using namespace pl::modmenu;

    ConfigSchemaBuilder schema;
    schema.defaultCategory("book_boundary_lab")
        .category("book_boundary_lab", "Book Boundary Lab",
                  "Local diagnostics for book page and text limits. Estimates only; no book is created or transmitted.");

    ConfigNodeV2 pages;
    pages.id = "m_targetPages";
    pages.key = "m_targetPages";
    pages.title = "Target page count";
    pages.description = "Hypothetical page count to analyze (1–256).";
    pages.category = "book_boundary_lab";
    pages.type = ConfigControlTypeV2::SliderFloat;
    pages.minValue = "1";
    pages.maxValue = "256";
    pages.step = "1";
    pages.defaultValue = "67";
    pages.currentValue = std::to_string(m_targetPages);
    pages.hasCurrentValue = true;
    schema.node(std::move(pages));

    ConfigNodeV2 codePoints;
    codePoints.id = "m_codePointsPerPage";
    codePoints.key = "m_codePointsPerPage";
    codePoints.title = "Unicode code points per page";
    codePoints.description = "Hypothetical Unicode scalar count per page (1–4096); this is not a write-limit bypass.";
    codePoints.category = "book_boundary_lab";
    codePoints.type = ConfigControlTypeV2::SliderFloat;
    codePoints.minValue = "1";
    codePoints.maxValue = "4096";
    codePoints.step = "1";
    codePoints.defaultValue = "1000";
    codePoints.currentValue = std::to_string(m_codePointsPerPage);
    codePoints.hasCurrentValue = true;
    schema.node(std::move(codePoints));

    ConfigNodeV2 run;
    run.id = "m_runEstimate";
    run.key = "m_runEstimate";
    run.title = "Run local estimate";
    run.description = "Writes a diagnostic summary to the BedAim log. Does not generate, save, or send a book.";
    run.category = "book_boundary_lab";
    run.type = ConfigControlTypeV2::Toggle;
    run.defaultValue = "false";
    run.currentValue = "false";
    run.hasCurrentValue = true;
    schema.node(std::move(run));

    setConfigSchemaJson(moduleId, schema.toJson());
}

bool BookBoundaryLabModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "m_targetPages") {
        return parseBoundedInteger(value, 1, 256, m_targetPages);
    }
    if (key == "m_codePointsPerPage") {
        return parseBoundedInteger(value, 1, 4096, m_codePointsPerPage);
    }
    if (key == "m_runEstimate") {
        if (value == "true" || value == "1" || value == "on") logEstimate();
        return value == "true" || value == "false" || value == "1" || value == "0" ||
               value == "on" || value == "off";
    }
    return false;
}

void BookBoundaryLabModule::logEstimate() const {
    const std::uint64_t totalCodePoints =
        static_cast<std::uint64_t>(m_targetPages) *
        static_cast<std::uint64_t>(m_codePointsPerPage);

    // Four bytes is the maximum UTF-8 width of one Unicode scalar value.
    const std::uint64_t maxUtf8Bytes = totalCodePoints * 4u;

    const std::string message =
        "Local estimate only: pages=" + std::to_string(m_targetPages) +
        ", code_points_per_page=" + std::to_string(m_codePointsPerPage) +
        ", total_code_points=" + std::to_string(totalCodePoints) +
        ", max_utf8_bytes=" + std::to_string(maxUtf8Bytes) +
        ". No book generated; no network or inventory action performed.";

    bedrocktools::diagnostics::log(
        bedrocktools::diagnostics::Level::Info, moduleId, message);
}

void BookBoundaryLabModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_targetPages = std::clamp(j.value("m_targetPages", m_targetPages), 1, 256);
    m_codePointsPerPage = std::clamp(j.value("m_codePointsPerPage", m_codePointsPerPage), 1, 4096);
}

void BookBoundaryLabModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_targetPages"] = m_targetPages;
    j["m_codePointsPerPage"] = m_codePointsPerPage;
}
