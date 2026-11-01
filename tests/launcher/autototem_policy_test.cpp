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
    return {true, true, false, true, false, false, false, true, true};
}
}

int main() {
    using bedaim::autototem::Decision;
    using bedaim::autototem::decide;
    using bedaim::autototem::normalizeDelayMs;
    using bedaim::autototem::shouldRetryDispatch;

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

    check(!shouldRetryDispatch(true, 1),
          "accepted Key Mapper broadcast is never resent automatically");
    check(shouldRetryDispatch(false, 1),
          "first failed Key Mapper broadcast schedules a retry");
    check(shouldRetryDispatch(false, 2),
          "second failed Key Mapper broadcast schedules the final retry");
    check(!shouldRetryDispatch(false, 3),
          "third failed Key Mapper broadcast exhausts the retry budget");
    check(!shouldRetryDispatch(false, 0),
          "retry requires an actual failed dispatch attempt");

    check(!bedaim::autototem::confirmationTimedOut(false, false, true),
          "no confirmation timeout exists without an accepted pending broadcast");
    check(!bedaim::autototem::confirmationTimedOut(true, true, true),
          "observed offhand totem completes confirmation");
    check(!bedaim::autototem::confirmationTimedOut(true, false, false),
          "confirmation window remains open before its deadline");
    check(bedaim::autototem::confirmationTimedOut(true, false, true),
          "missing offhand change is reported after confirmation deadline");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
