#include "modules/player/PlayerGameType.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

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
void expectNative(std::int32_t nativeValue, std::int32_t expected,
                  std::string_view label) {
    std::int32_t canonical = -99;
    check(bedaim::playergametype::normalizeNativeValue(nativeValue, canonical),
          label);
    check(canonical == expected, "native GameType maps to expected canonical id");
}
}

int main() {
    expectNative(0, 0, "Survival is recognized");
    expectNative(1, 1, "Creative is recognized");
    expectNative(2, 2, "Adventure is recognized");
    expectNative(6, 3, "Spectator native enum value 6 maps to canonical id 3");

    for (const std::int32_t unknown : {-1, 3, 4, 5, 7, 99}) {
        std::int32_t canonical = -99;
        check(!bedaim::playergametype::normalizeNativeValue(unknown, canonical),
              "unknown/default/network enum value is rejected instead of guessed");
        check(canonical == -99, "failed normalization leaves output unchanged");
    }

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
