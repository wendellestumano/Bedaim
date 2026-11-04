#include "modules/misc/BookPaginator.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures = 0;
int checks = 0;

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
}

int main() {
    using bedaim::books::paginate;

    const auto basic = paginate("hello world", 7, 10);
    check(basic.ok, "basic text paginates");
    check(basic.pages.size() == 2, "whitespace boundary creates two pages");
    check(basic.pages[0].text == "hello ", "first page preserves separator");
    check(basic.pages[1].text == "world", "second page preserves remaining word");
    check(basic.pages[0].text + basic.pages[1].text == "hello world",
          "pagination preserves exact source text");

    const std::string unicode = "A😀B";
    const auto emoji = paginate(unicode, 2, 10);
    check(emoji.ok && emoji.pages.size() == 2, "unicode splits on code-point boundaries");
    check(emoji.totalCodePoints == 3, "emoji counts as one Unicode code point");
    check(emoji.totalUtf16Units == 4, "emoji counts as two UTF-16 units");
    check(emoji.totalUtf8Bytes == 6, "emoji UTF-8 byte count is correct");
    std::string reconstructed;
    for (const auto& page : emoji.pages) reconstructed += page.text;
    check(reconstructed == unicode, "unicode bytes are preserved");

    const auto longWord = paginate("abcdefgh", 3, 10);
    check(longWord.ok && longWord.pages.size() == 3, "long words use safe hard split");
    reconstructed.clear();
    for (const auto& page : longWord.pages) reconstructed += page.text;
    check(reconstructed == "abcdefgh", "hard split loses no characters");

    const std::string invalid("\xC0\xAF", 2);
    const auto malformed = paginate(invalid, 10, 10);
    check(!malformed.ok && malformed.pages.empty(), "malformed UTF-8 is rejected");

    const auto tooMany = paginate("abcdefgh", 2, 2);
    check(!tooMany.ok && tooMany.pages.empty(), "page cap rejects incomplete plan");

    const auto empty = paginate("", 10, 10);
    check(empty.ok && empty.pages.empty(), "empty input produces an empty valid plan");

    // Regression for the expanded local planner: 200 chunks of 256 ASCII code points.
    const std::string twoHundredPages(200u * 256u, 'x');
    const auto expanded = paginate(twoHundredPages, 256, 200);
    check(expanded.ok, "expanded planner accepts a 200-page local plan");
    check(expanded.pages.size() == 200, "expanded planner produces exactly 200 pages");
    bool pageSizesValid = expanded.pages.size() == 200;
    reconstructed.clear();
    for (const auto& page : expanded.pages) {
        pageSizesValid = pageSizesValid && page.codePoints <= 256;
        reconstructed += page.text;
    }
    check(pageSizesValid, "all expanded plan pages stay within the configured 256-code-point budget");
    check(reconstructed == twoHundredPages, "expanded 200-page plan preserves every input byte");

    const auto overExpanded = paginate(std::string(201u * 256u, 'x'), 256, 200);
    check(!overExpanded.ok && overExpanded.pages.empty(), "expanded planner rejects page 201 without partial output");

    const auto largerInput = paginate(std::string(32u * 1024u, 'z'), 256, 200);
    check(largerInput.ok && largerInput.pages.size() == 128,
          "expanded planner accepts input larger than the former 8 KiB cap");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
