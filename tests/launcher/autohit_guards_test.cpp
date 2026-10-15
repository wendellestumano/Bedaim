#include "modules/player/AutoHitGuards.hpp"

#include <cstdlib>
#include <cmath>
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

bedaim::autohit::Inputs validInputs() {
    return {
        true, true, true, true, true, true, true, true, true, 2.5f, 3.0f
    };
}
}

int main() {
    using bedaim::autohit::Decision;
    using bedaim::autohit::evaluate;
    using bedaim::autohit::effectiveScanRange;

    check(effectiveScanRange(5.0f, false, 6.0f) == 5.0f,
          "AutoHit disabled preserves Aim Assist scan range");
    check(effectiveScanRange(2.0f, true, 6.0f) == 6.0f,
          "AutoHit query reaches the configured AutoHit range");
    check(effectiveScanRange(10.0f, true, 6.0f) == 10.0f,
          "AutoHit never shrinks a larger Aim Assist scan range");
    check(effectiveScanRange(100.0f, true, 100.0f) == 80.0f,
          "effective scan range is capped at 80 blocks");
    check(effectiveScanRange(2.0f, false, 6.0f, 4.0f) == 6.0f,
          "expanded target geometry adds a bounded query margin");
    check(effectiveScanRange(79.0f, true, 6.0f, 4.0f) == 80.0f,
          "query expansion margin cannot push scan beyond 80 blocks");
    check(effectiveScanRange(std::numeric_limits<float>::quiet_NaN(), true, 3.0f) == 5.5f,
          "non-finite Aim Assist range falls back to the safe default");

    check(evaluate(validInputs()) == Decision::Ready,
          "all valid prerequisites allow the attack attempt");

    auto input = validInputs();
    input.enabled = false;
    check(evaluate(input) == Decision::Disabled,
          "disabled AutoHit never dispatches");

    input = validInputs(); input.localPlayerValid = false;
    check(evaluate(input) == Decision::MissingPlayer,
          "missing or mismatched local player blocks dispatch");
    input = validInputs(); input.targetPresent = false;
    check(evaluate(input) == Decision::MissingTarget,
          "missing target blocks dispatch");
    input = validInputs(); input.boundsValid = false;
    check(evaluate(input) == Decision::InvalidBounds,
          "invalid target bounds block dispatch");
    input = validInputs(); input.targetFresh = false;
    check(evaluate(input) == Decision::StaleTarget,
          "stale target snapshot blocks dispatch");
    input = validInputs(); input.gameModeValid = false;
    check(evaluate(input) == Decision::MissingGameMode,
          "missing GameMode blocks dispatch");
    input = validInputs(); input.attackRouteAvailable = false;
    check(evaluate(input) == Decision::MissingAttackRoute,
          "unvalidated native attack route blocks dispatch");
    input = validInputs(); input.hitPointFinite = false;
    check(evaluate(input) == Decision::InvalidHitPoint,
          "invalid hit point blocks dispatch");
    input = validInputs(); input.distance = 3.01f;
    check(evaluate(input) == Decision::OutOfRange,
          "hit point beyond configured range blocks dispatch");
    input = validInputs(); input.distance = std::numeric_limits<float>::quiet_NaN();
    check(evaluate(input) == Decision::InvalidHitPoint,
          "non-finite hit distance blocks dispatch");
    input = validInputs(); input.maxDistance = std::numeric_limits<float>::infinity();
    check(evaluate(input) == Decision::InvalidHitPoint,
          "non-finite attack range blocks dispatch");
    input = validInputs(); input.cooldownReady = false;
    check(evaluate(input) == Decision::Cooldown,
          "active cooldown blocks dispatch");
    input = validInputs(); input.distance = 3.0f;
    check(evaluate(input) == Decision::Ready,
          "target exactly at maximum range is permitted");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
