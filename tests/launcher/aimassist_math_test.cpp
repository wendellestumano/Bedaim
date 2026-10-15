#include "modules/player/AimAssistMath.hpp"

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
    using bedaim::aimmath::Bounds;
    using bedaim::aimmath::Vec3;
    using bedaim::aimmath::boundedTurnStep;
    using bedaim::aimmath::isFiniteVector;
    using bedaim::aimmath::isValidBounds;
    using bedaim::aimmath::wrapDegrees;

    const float wrapAcrossBoundary = wrapDegrees(-179.0f - 179.0f);
    check(std::abs(wrapAcrossBoundary - 2.0f) < 1.0e-4f,
          "yaw crossing +179 to -179 uses the short +2 degree turn");
    check(std::abs(wrapDegrees(181.0f) + 179.0f) < 1.0e-4f,
          "angles above +180 wrap into the canonical interval");
    check(std::abs(wrapDegrees(-181.0f) - 179.0f) < 1.0e-4f,
          "angles below -180 wrap into the canonical interval");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    check(!isFiniteVector(Vec3{nan, 0.0f, 0.0f}),
          "non-finite vector components are rejected");
    check(!isFiniteVector(Vec3{0.0f, inf, 0.0f}),
          "infinite vector components are rejected");

    check(isValidBounds(Bounds{{-0.3f, 0.0f, -0.3f}, {0.3f, 1.8f, 0.3f}}),
          "ordinary actor bounds are accepted");
    check(!isValidBounds(Bounds{{0.3f, 0.0f, -0.3f}, {-0.3f, 1.8f, 0.3f}}),
          "inverted x bounds are rejected");
    check(!isValidBounds(Bounds{{-0.3f, 1.0f, -0.3f}, {0.3f, 1.0f, 0.3f}}),
          "zero-height bounds are rejected");
    check(!isValidBounds(Bounds{{-0.3f, 0.0f, -0.3f}, {nan, 1.8f, 0.3f}}),
          "bounds with non-finite coordinates are rejected");

    const float turn = boundedTurnStep(90.0f, 120.0f, 1.0f / 60.0f);
    check(std::isfinite(turn) && turn <= 2.0001f && turn > 0.0f,
          "turn step never exceeds turn rate times frame delta");
    check(std::abs(boundedTurnStep(-90.0f, 120.0f, 1.0f / 60.0f) + turn) < 1.0e-4f,
          "negative turn direction is symmetric");
    check(std::abs(boundedTurnStep(90.0f, 120.0f, 1.0f) - 6.0f) < 1.0e-4f,
          "frame delta is clamped to at most 50 milliseconds");
    check(boundedTurnStep(nan, 120.0f, 1.0f / 60.0f) == 0.0f,
          "non-finite angular error produces no camera delta");
    check(boundedTurnStep(90.0f, nan, 1.0f / 60.0f) == 0.0f,
          "non-finite turn rate produces no camera delta");

    const float nearScale = bedaim::aimmath::distanceHitboxScale(1.0f, 0.5f, 30.0f, 5.0f, true);
    const float mediumScale = bedaim::aimmath::distanceHitboxScale(1.0f, 10.0f, 30.0f, 5.0f, true);
    const float farScale = bedaim::aimmath::distanceHitboxScale(1.0f, 30.0f, 30.0f, 5.0f, true);
    check(nearScale < 1.0f, "distance hitbox is smaller than native near the player");
    check(mediumScale > nearScale && mediumScale < farScale,
          "distance hitbox grows smoothly between near and far ranges");
    check(farScale > 2.0f && farScale < 4.0f,
          "default distance strength provides visible far-range expansion");
    const float normalRangeFarScale =
        bedaim::aimmath::distanceHitboxScale(1.0f, 5.5f, 5.5f, 5.0f, true);
    check(normalRangeFarScale > 2.0f && normalRangeFarScale < 4.0f,
          "default strength still expands at normal 5.5-block aim range");
    check(std::abs(bedaim::aimmath::distanceHitboxScale(2.0f, 30.0f, 30.0f, 5.0f, false) - 2.0f) < 1.0e-4f,
          "disabled distance curve uses the base hitbox scale");
    check(std::isfinite(bedaim::aimmath::distanceHitboxScale(1.0f,
              std::numeric_limits<float>::infinity(), 80.0f, 200.0f, true)),
          "non-finite distance does not poison the hitbox scale");
    check(bedaim::aimmath::distanceHitboxScale(1.0f, 80.0f, 80.0f, 200.0f, true) <= 12.0f,
          "far-range hitbox multiplier remains bounded");

    std::cout << checks << " checks, " << failures << " failures\\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
