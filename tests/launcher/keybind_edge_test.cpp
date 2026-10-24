#include "core/KeybindEdge.hpp"
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
int checks = 0;
int failures = 0;
void check(bool condition, std::string_view label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}
}

int main() {
    bedrocktools::core::KeybindEdge edge;
    check(edge.update(true), "first key-down is one press edge");
    check(!edge.update(true), "repeated key-down does not create another edge");
    check(!edge.update(true), "held key remains latched across repeats");
    check(!edge.update(false), "key-up releases without toggling");
    check(edge.update(true), "press after release produces a new edge");
    check(!edge.update(false), "release does not report a toggle edge");
    check(!edge.update(false), "duplicate release leaves the latch released");
    check(edge.update(true), "next independent press is accepted");
    edge.reset();
    check(edge.update(true), "explicit reset clears stale held state");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}