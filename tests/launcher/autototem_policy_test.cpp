#include "modules/player/AutoTotemPolicy.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
int checks = 0;
int failures = 0;

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}

bedaim::autototem::Inputs ready() {
    return {true, true, false, true, true, false, true, true, true};
}
}

int main() {
    using bedaim::autototem::Decision;
    using bedaim::autototem::decide;
    using bedaim::autototem::normalizeDelayMs;

    auto state = ready();
    check(decide(state) == Decision::Equip, "valid pending refill is allowed");

    state.enabled = false;
    check(decide(state) == Decision::Idle, "disabled module does nothing");

    state = ready(); state.pending = false;
    check(decide(state) == Decision::Idle, "no refill request does nothing");

    state = ready(); state.offhandHasTotem = true;
    check(decide(state) == Decision::Idle, "already equipped totem is never replaced");

    state = ready(); state.containerOpen = true;
    check(decide(state) == Decision::WaitForContainer, "wait while player inventory UI is open");

    state = ready(); state.combatOnly = true; state.inCombat = false;
    check(decide(state) == Decision::WaitForCombat, "combat-only mode waits for combat");

    state = ready(); state.delayElapsed = false;
    check(decide(state) == Decision::WaitForDelay, "configured post-consumption delay is honored");

    state = ready(); state.candidateAvailable = false;
    check(decide(state) == Decision::WaitForItem, "retry when no inventory totem is available");

    state = ready(); state.offhandEmpty = false;
    check(decide(state) == Decision::WaitForOffhandClear, "never overwrite a non-totem offhand item");

    check(normalizeDelayMs(0.0f) == 0.0f, "zero delay is supported");
    check(normalizeDelayMs(100.0f) == 100.0f, "default delay stays 100 ms");
    check(normalizeDelayMs(1500.0f) == 1000.0f, "delay is capped at 1000 ms");
    check(normalizeDelayMs(-5.0f) == 0.0f, "negative delay is clamped to zero");
    check(normalizeDelayMs(std::numeric_limits<float>::quiet_NaN()) == 100.0f,
          "non-finite delay returns the safe default");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
