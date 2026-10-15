#include "modules/player/AutoHitKeyBinding.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace {
int failures = 0;
int checks = 0;

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}
}

int main() {
    using bedaim::autohitkey::format;
    using bedaim::autohitkey::parse;

    for (const auto& [text, expected] : {
             std::pair<std::string_view, int>{"auto", 0},
             {"off", 0}, {"0", 7}, {"1", 8}, {"9", 16},
             {"a", 29}, {"Z", 54}, {"shift", 59}, {"shift right", 60},
             {"ctrl", 113}, {"space", 62}, {"enter", 66}, {"esc", 111},
             {"button1", 188}, {"Button 16", 203}, {"keycode:120", 120}}) {
        const auto value = parse(text);
        check(value.has_value(), "valid key alias is accepted");
        check(value && *value == expected, "key alias maps to the expected Android key code");
    }

    check(!parse("button17").has_value(), "unsupported button index is rejected");
    check(!parse("shiftplease").has_value(), "unknown alias is rejected");
    check(!parse("keycode:999").has_value(), "out-of-range key code is rejected");
    check(format(7) == "0", "KEYCODE_0 formats as a user-friendly zero");
    check(format(59) == "shift", "shift code formats as shift");
    check(format(188) == "button1", "button one code formats as button1");
    check(format(0) == "auto", "zero code formats as auto mode");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
