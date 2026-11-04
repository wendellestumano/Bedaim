#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bedaim::books {

struct Page {
    std::string text;
    std::size_t codePoints = 0;
    std::size_t utf8Bytes = 0;
    std::size_t utf16Units = 0;
};

struct PaginationResult {
    bool ok = false;
    std::string error;
    std::vector<Page> pages;
    std::size_t totalCodePoints = 0;
    std::size_t totalUtf8Bytes = 0;
    std::size_t totalUtf16Units = 0;
};

inline constexpr int kMaxPlannedPages = 200;

// Parse and normalize the module's page-cap setting in one place so the UI
// callback and persisted configuration cannot disagree about the upper bound.
inline bool parsePageCountSetting(std::string_view value, int& output) {
    try {
        const std::string text(value);
        std::size_t used = 0;
        const float parsed = std::stof(text, &used);
        if (used != text.size() || !std::isfinite(parsed)) return false;
        output = static_cast<int>(std::lround(
            std::clamp(parsed, 1.0f, static_cast<float>(kMaxPlannedPages))));
        return true;
    } catch (...) {
        return false;
    }
}

// Move a cursor within a local page plan. The UI may keep its Next button
// visible at every page; this helper prevents overflow and clamps at either end.
inline bool movePageCursor(std::size_t& index, std::size_t pageCount, int direction) {
    if (pageCount == 0 || direction == 0) return false;
    if (direction > 0) {
        if (index + 1 >= pageCount) return false;
        ++index;
        return true;
    }
    if (index == 0 || index >= pageCount) return false;
    --index;
    return true;
}

inline bool decodeUtf8(std::string_view text, std::vector<std::size_t>& ends,
                       std::vector<std::size_t>& utf16Widths) {
    ends.clear();
    utf16Widths.clear();
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t width = 0;
        std::uint32_t codePoint = 0;
        if (lead <= 0x7F) {
            width = 1;
            codePoint = lead;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            width = 2;
            codePoint = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            width = 3;
            codePoint = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            width = 4;
            codePoint = lead & 0x07;
        } else {
            return false;
        }
        if (i + width > text.size()) return false;
        for (std::size_t j = 1; j < width; ++j) {
            const auto continuation = static_cast<unsigned char>(text[i + j]);
            if ((continuation & 0xC0) != 0x80) return false;
            codePoint = (codePoint << 6) | (continuation & 0x3F);
        }
        if ((width == 3 && codePoint < 0x800) ||
            (width == 4 && codePoint < 0x10000) ||
            (codePoint >= 0xD800 && codePoint <= 0xDFFF) ||
            codePoint > 0x10FFFF) {
            return false;
        }
        i += width;
        ends.push_back(i);
        utf16Widths.push_back(codePoint > 0xFFFF ? 2u : 1u);
    }
    return true;
}

inline bool isPreferredBreak(std::string_view text, std::size_t begin,
                             std::size_t end) {
    if (begin >= end) return false;
    const auto value = static_cast<unsigned char>(text[begin]);
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

// Splits local text into bounded pages. It preserves every UTF-8 byte and prefers
// whitespace boundaries. This is a text planner, not a native-limit bypass.
inline PaginationResult paginate(std::string_view text,
                                 std::size_t maxCodePointsPerPage,
                                 std::size_t maxPages,
                                 std::size_t maxInputBytes = 65536) {
    PaginationResult result;
    if (text.size() > maxInputBytes) {
        result.error = "Input exceeds the local planner byte limit.";
        return result;
    }
    if (maxCodePointsPerPage == 0 || maxPages == 0) {
        result.error = "Page and page-count limits must be greater than zero.";
        return result;
    }

    std::vector<std::size_t> ends;
    std::vector<std::size_t> utf16Widths;
    if (!decodeUtf8(text, ends, utf16Widths)) {
        result.error = "Input is not valid UTF-8; no pages were produced.";
        return result;
    }

    result.totalCodePoints = ends.size();
    result.totalUtf8Bytes = text.size();
    for (const auto width : utf16Widths) result.totalUtf16Units += width;
    if (text.empty()) {
        result.ok = true;
        return result;
    }

    std::size_t cursor = 0;
    while (cursor < ends.size()) {
        std::size_t end = cursor + maxCodePointsPerPage;
        if (end > ends.size()) end = ends.size();

        if (end < ends.size()) {
            std::size_t preferred = end;
            while (preferred > cursor) {
                const std::size_t candidate = preferred - 1;
                const std::size_t byteBegin = candidate == 0 ? 0 : ends[candidate - 1];
                if (isPreferredBreak(text, byteBegin, ends[candidate])) {
                    preferred = candidate + 1;
                    break;
                }
                --preferred;
            }
            if (preferred > cursor && preferred < end) end = preferred;
        }

        if (result.pages.size() >= maxPages) {
            result.pages.clear();
            result.error = "Text needs more pages than the configured local page-plan cap.";
            return result;
        }

        const std::size_t byteBegin = cursor == 0 ? 0 : ends[cursor - 1];
        const std::size_t byteEnd = ends[end - 1];
        Page page;
        page.text.assign(text.substr(byteBegin, byteEnd - byteBegin));
        page.codePoints = end - cursor;
        page.utf8Bytes = byteEnd - byteBegin;
        for (std::size_t i = cursor; i < end; ++i) {
            page.utf16Units += utf16Widths[i];
        }
        result.pages.push_back(std::move(page));
        cursor = end;
    }

    result.ok = true;
    return result;
}

} // namespace bedaim::books
