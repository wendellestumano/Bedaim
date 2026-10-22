#include "modules/player/MaceFallPolicy.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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
}

int main() {
    using bedaim::macefall::shouldUnglide;
    check(shouldUnglide(true, true, 5.0f, 8.0f, 2.0f, 4.0f),
          "Unglide triggers when target and floor are inside configured limits");
    check(!shouldUnglide(false, true, 5.0f, 8.0f, 2.0f, 4.0f),
          "disabled Unglide never triggers");
    check(!shouldUnglide(true, false, 5.0f, 8.0f, 2.0f, 4.0f),
          "Unglide requires actual Elytra gliding");
    check(!shouldUnglide(true, true, 8.01f, 8.0f, 2.0f, 4.0f),
          "target just beyond configured distance does not trigger");
    check(!shouldUnglide(true, true, 5.0f, 8.0f, 4.01f, 4.0f),
          "floor clearance above configured height does not trigger");
    check(shouldUnglide(true, true, 8.0f, 8.0f, 4.0f, 4.0f),
          "exact distance and ground-clearance boundaries are inclusive");
    check(!shouldUnglide(true, true, std::numeric_limits<float>::infinity(),
                         8.0f, 2.0f, 4.0f),
          "invalid target distance fails closed");
    check(!shouldUnglide(true, true, 5.0f, 8.0f,
                         std::numeric_limits<float>::infinity(), 4.0f),
          "unknown ground clearance fails closed");
    check(!shouldUnglide(true, true, -1.0f, 8.0f, 2.0f, 4.0f),
          "negative target distance fails closed");
    check(shouldUnglide(true, true, 30.0f, 100.0f, 23.0f, 100.0f),
          "configured thresholds are bounded to supported slider maxima");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
