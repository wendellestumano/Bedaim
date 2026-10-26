#include "modules/player/ElytraMacingPolicy.hpp"

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
    using namespace bedaim::elytramacing;

    check(normalizeEngageDistance(4.5f) == 4.5f,
          "configured engagement distance is preserved");
    check(normalizeEngageDistance(0.0f) == 2.0f,
          "engagement distance clamps to minimum");
    check(normalizeEngageDistance(std::numeric_limits<float>::quiet_NaN()) == 4.5f,
          "non-finite engagement distance falls back to safe default");

    using bedaim::elytramacing::shouldAimAboveTarget;
    check(shouldAimAboveTarget(true, true, true, 8.0f, 4.5f, true),
          "gliding chase aims above a distant target");
    check(!shouldAimAboveTarget(true, false, true, 8.0f, 4.5f, true),
          "no approach offset while not gliding");
    check(!shouldAimAboveTarget(true, true, false, 8.0f, 4.5f, true),
          "no approach offset without a valid target");
    check(!shouldAimAboveTarget(true, true, true, 4.5f, 4.5f, true),
          "approach offset resets at the engagement boundary");
    check(!shouldAimAboveTarget(true, true, true, 8.0f, 4.5f, false),
          "approach offset is disabled outside the chase phase");

    check(shouldBoostTowardTarget(true, true, true, 8.0f, 4.5f, true),
          "gliding player boosts toward a valid distant target");
    check(!shouldBoostTowardTarget(true, false, true, 8.0f, 4.5f, true),
          "boost is not requested while not gliding");
    check(!shouldBoostTowardTarget(true, true, false, 8.0f, 4.5f, true),
          "boost is not requested without a valid target");
    check(!shouldBoostTowardTarget(true, true, true, 4.5f, 4.5f, true),
          "approach boost stops at the engagement boundary");
    check(!shouldBoostTowardTarget(true, true, true, 8.0f, 4.5f, false),
          "rocket cooldown blocks another boost");
    check(!shouldBoostTowardTarget(true, true, true,
          std::numeric_limits<float>::infinity(), 4.5f, true),
          "invalid target distance fails closed");

    check(shouldDisengageForMace(true, true, true, 3.0f, 4.5f, true, true),
          "near aligned target and available mace allow glide stop");
    check(!shouldDisengageForMace(true, true, true, 3.0f, 4.5f, false, true),
          "mace engagement requires an available mace and AutoHit");
    check(!shouldDisengageForMace(true, true, true, 3.0f, 4.5f, true, false),
          "mace engagement requires an unglide keymap UID");
    check(!shouldDisengageForMace(true, false, true, 3.0f, 4.5f, true, true),
          "glide-stop action is not repeated when already not gliding");
    check(!shouldDisengageForMace(true, true, true, 5.0f, 4.5f, true, true),
          "mace engagement waits until the configured distance");

    check(shouldRequestGlide(true, false, false, -0.2f, true, true),
          "falling player can request the double-jump glide macro");
    check(shouldRequestGlide(true, false, true, 0.0f, true, true),
          "grounded player can start the launch sequence without waiting to fall");
    check(!shouldRequestGlide(true, false, false, 0.0f, true, true),
          "level/stationary airborne player does not trigger launch or fall recovery");
    check(!shouldRequestGlide(true, false, false, 0.2f, true, true),
          "upward airborne motion does not trigger a false launch at the apex");
    check(!shouldRequestGlide(true, true, true, -0.2f, true, true),
          "already-gliding player does not retrigger start-glide macro");
    check(!shouldRequestGlide(true, false, true, 0.0f, false, true),
          "start-glide action requires a configured keymap");
    check(!shouldRequestGlide(true, false, true, 0.0f, true, false),
          "glide retry cooldown is respected");

    check(detectedWindBurstRise(true, false, 0.2f),
          "upward impulse during an attack window is detected");
    check(!detectedWindBurstRise(false, false, 0.2f),
          "upward motion outside the mace window is not called Wind Burst");
    check(!detectedWindBurstRise(true, true, 0.2f),
          "already-gliding motion does not trigger recovery");
    check(!detectedWindBurstRise(true, false, -0.2f),
          "falling motion is not classified as an upward impulse");

    check(detectedLandingRecovery(true, false, true, 5),
          "stable ticks after a fall start ground recovery");
    check(!detectedLandingRecovery(true, true, true, 5),
          "ground recovery is not triggered while gliding");
    check(!detectedLandingRecovery(true, false, false, 5),
          "ground recovery requires an observed fall");
    check(!detectedLandingRecovery(true, false, true, 2),
          "briefly stable motion does not count as landing");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
