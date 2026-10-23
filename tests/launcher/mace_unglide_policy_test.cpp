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
    using bedaim::macefall::shouldPulseUnglideJump;
    using bedaim::macefall::shouldTriggerUnglideKeymap;
    check(shouldPulseUnglideJump(true, true, 5.0f, 8.0f),
          "Unglide pulses jump when another player is within configured distance");
    check(!shouldPulseUnglideJump(false, true, 5.0f, 8.0f),
          "disabled Unglide never pulses jump");
    check(!shouldPulseUnglideJump(true, false, 5.0f, 8.0f),
          "Unglide requires actual Elytra gliding");
    check(!shouldPulseUnglideJump(true, true, 8.01f, 8.0f),
          "target just beyond configured distance does not trigger");
    check(shouldPulseUnglideJump(true, true, 8.0f, 8.0f),
          "exact target-distance boundary is inclusive");
    check(shouldTriggerUnglideKeymap(true, true, true, 5.0f, 8.0f),
          "Unglide keymap accepts a player confirmed under the crosshair and in range");
    check(!shouldTriggerUnglideKeymap(true, true, false, 5.0f, 8.0f),
          "Unglide blocks keymap requests when the crosshair is not on target");
    check(!shouldTriggerUnglideKeymap(true, false, true, 5.0f, 8.0f),
          "Unglide only triggers for a player target");
    check(!shouldTriggerUnglideKeymap(true, true, true, 8.01f, 8.0f),
          "Unglide target policy blocks targets beyond configured range");

    using bedaim::macefall::shouldRearmUnglide;
    check(!shouldRearmUnglide(5.0f, 8.0f),
          "remaining inside trigger range does not re-arm another Unglide");
    check(!shouldRearmUnglide(9.0f, 8.0f),
          "hysteresis prevents rapid re-arm at the edge of range");
    check(shouldRearmUnglide(9.6f, 8.0f),
          "leaving trigger range plus hysteresis re-arms the next attack");
    check(!shouldRearmUnglide(std::numeric_limits<float>::infinity(), 8.0f),
          "invalid distance cannot re-arm Unglide");
    check(!shouldPulseUnglideJump(true, true, std::numeric_limits<float>::infinity(), 8.0f),
          "invalid target distance fails closed");
    check(!shouldPulseUnglideJump(true, true, -1.0f, 8.0f),
          "negative target distance fails closed");
    check(shouldPulseUnglideJump(true, true, 30.0f, 100.0f),
          "configured target distance is clamped to supported slider maximum");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
