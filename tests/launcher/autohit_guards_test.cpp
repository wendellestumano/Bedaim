#include "modules/player/AutoHitGuards.hpp"
#include "modules/player/PlayerGameType.hpp"
#include "modules/player/MaceFallPolicy.hpp"

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

    std::int32_t canonicalMode = -1;
    check(bedaim::playergametype::normalizeNativeValue(1, false, canonicalMode) && canonicalMode == 0,
          "legacy GameType 1 normalizes to Survival");
    check(bedaim::playergametype::normalizeNativeValue(2, false, canonicalMode) && canonicalMode == 1,
          "legacy GameType 2 normalizes to Creative");
    check(bedaim::playergametype::normalizeNativeValue(3, false, canonicalMode) && canonicalMode == 2,
          "legacy GameType 3 normalizes to Adventure");
    check(bedaim::playergametype::normalizeNativeValue(5, false, canonicalMode) && canonicalMode == 3,
          "legacy GameType 5 normalizes to Spectator");
    check(!bedaim::playergametype::normalizeNativeValue(0, false, canonicalMode),
          "legacy undefined GameType isn't Survival");
    check(bedaim::playergametype::normalizeNativeValue(0, true, canonicalMode) && canonicalMode == 0,
          "shifted GameType 0 normalizes to Survival");
    check(bedaim::playergametype::normalizeNativeValue(6, true, canonicalMode) && canonicalMode == 3,
          "shifted GameType 6 normalizes to Spectator");
    check(!bedaim::playergametype::normalizeNativeValue(5, true, canonicalMode),
          "shifted Default isn't Spectator");

    check(bedaim::macefall::shouldBeginMaceAction(-0.10f, 3.0f, 3.0f),
          "Mace accepts a confirmed fall at the configured minimum height");
    check(!bedaim::macefall::shouldBeginMaceAction(-0.10f, 2.99f, 3.0f),
          "Mace rejects a fall shorter than the configured minimum");
    check(!bedaim::macefall::shouldBeginMaceAction(-0.01f, 4.0f, 1.0f),
          "Mace rejects near-level motion despite stale height");
    check(bedaim::macefall::shouldBeginMaceAction(-0.10f, 4.0f, std::numeric_limits<float>::quiet_NaN()),
          "Mace normalizes a non-finite minimum to its safe default");

    check(bedaim::autohit::publicAttackRouteAvailable(true),
          "validated public attack wrapper allows dispatch");
    check(!bedaim::autohit::publicAttackRouteAvailable(false),
          "unvalidated public attack wrapper blocks dispatch");

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
