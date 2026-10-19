#include "modules/player/AutoHitGuards.hpp"
#include "core/NativeAttackResultPolicy.hpp"

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
    using bedrocktools::core::attackresult::confirmedByNativeRoute;

    check(confirmedByNativeRoute(true, true, false, false, false),
          "three-argument route uses the observed internal result");
    check(confirmedByNativeRoute(true, true, true, true, true),
          "four-argument route confirms when its native output flag is true");
    check(!confirmedByNativeRoute(true, true, true, true, false),
          "four-argument route rejects internal true with output flag false");
    check(!confirmedByNativeRoute(true, true, true, false, true),
          "four-argument route rejects an output flag that was not observed as used");
    check(!confirmedByNativeRoute(false, true, true, true, true),
          "missing internal observer never confirms an attack result");
    check(!confirmedByNativeRoute(true, false, true, true, true),
          "false internal return never confirms an attack result");

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

    bedaim::autohit::NoHitFeedbackGuard noHitGuard;
    const void* firstTarget = reinterpret_cast<const void*>(0x1000);
    const void* secondTarget = reinterpret_cast<const void*>(0x2000);
    check(noHitGuard.consecutiveNoFeedback() == 0,
          "no-hit feedback tracker starts empty");
    for (std::uint32_t i = 1; i < bedaim::autohit::NoHitFeedbackGuard::kAttemptsBeforeWarning; ++i) {
        check(!noHitGuard.observe(firstTarget, true, 0, 0),
              "no-hit tracker waits for its diagnostic warning threshold");
    }
    check(noHitGuard.observe(firstTarget, true, 0, 0),
          "six accepted calls without immediate hurt-time feedback emit one warning");
    check(!noHitGuard.observe(firstTarget, true, 0, 0),
          "missing immediate feedback does not repeatedly emit or block dispatch");
    check(noHitGuard.consecutiveNoFeedback() ==
              bedaim::autohit::NoHitFeedbackGuard::kAttemptsBeforeWarning,
          "feedback streak remains observable without disabling AutoHit");
    noHitGuard.observe(secondTarget, true, 0, 0);
    check(noHitGuard.consecutiveNoFeedback() == 1,
          "changing target identity starts a new diagnostic streak");

    check(!noHitGuard.observe(secondTarget, false, 0, 0),
          "a rejected native call breaks the no-feedback streak");
    check(noHitGuard.consecutiveNoFeedback() == 0,
          "rejected call resets the no-feedback counter");
    noHitGuard.observe(secondTarget, true, 0, 0);
    check(!noHitGuard.observe(secondTarget, true, 0, 10),
          "positive hurt-time transition resets the warning streak");
    check(noHitGuard.consecutiveNoFeedback() == 0,
          "positive hurt-time transition clears the streak");
    for (std::uint32_t i = 1; i < bedaim::autohit::NoHitFeedbackGuard::kAttemptsBeforeWarning; ++i)
        check(!noHitGuard.observe(secondTarget, true, 0, 0),
              "new no-feedback streak waits until its threshold");
    check(noHitGuard.observe(secondTarget, true, 0, 0),
          "new streak can emit a warning after a real reset");
    noHitGuard.reset();
    check(noHitGuard.consecutiveNoFeedback() == 0,
          "explicit reset clears diagnostic state");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
