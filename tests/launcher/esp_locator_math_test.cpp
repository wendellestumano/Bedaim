#include "modules/visual/EspLocatorMath.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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
    using bedaim::esplocator::Direction;
    using bedaim::esplocator::screenEdgeDirection;

    Direction direction{};
    constexpr float horizontalHalfFov = 85.0f;
    constexpr float verticalHalfFov = 56.5f;

    // Camera pitch < 0 means looking up. Targets below it must produce a
    // positive screen Y (the bottom edge), not the opposite top edge.
    check(screenEdgeDirection(0.0f, 20.0f, 0.0f, -70.0f,
                              horizontalHalfFov, verticalHalfFov, direction) &&
          direction.y > 0.0f,
          "looking up points down toward a lower target");

    check(screenEdgeDirection(0.0f, -85.0f, 0.0f, -35.0f,
                              horizontalHalfFov, verticalHalfFov, direction) &&
          direction.y < 0.0f,
          "looking up keeps a higher target on the upper side");

    check(screenEdgeDirection(0.0f, -20.0f, 0.0f, 70.0f,
                              horizontalHalfFov, verticalHalfFov, direction) &&
          direction.y < 0.0f,
          "looking down points up toward a higher target");

    check(screenEdgeDirection(0.0f, 85.0f, 0.0f, 35.0f,
                              horizontalHalfFov, verticalHalfFov, direction) &&
          direction.y > 0.0f,
          "looking down keeps a lower target on the lower side");

    check(screenEdgeDirection(35.0f, 0.0f, 0.0f, 0.0f,
                              horizontalHalfFov, verticalHalfFov, direction) &&
          direction.x > 0.0f,
          "positive yaw delta points right");

    check(screenEdgeDirection(-35.0f, 0.0f, 0.0f, 0.0f,
                              horizontalHalfFov, verticalHalfFov, direction) &&
          direction.x < 0.0f,
          "negative yaw delta points left");

    check(screenEdgeDirection(-179.0f, 0.0f, 179.0f, 0.0f,
                              horizontalHalfFov, verticalHalfFov, direction) &&
          direction.x > 0.0f,
          "yaw crossing the -180/180 boundary uses the short turn");

    check(screenEdgeDirection(180.0f, -80.0f, 0.0f, 80.0f,
                              horizontalHalfFov, verticalHalfFov, direction) &&
          (std::abs(direction.x) > 0.01f || std::abs(direction.y) > 0.01f),
          "targets behind a near-vertical camera retain a defined direction");

    check(!screenEdgeDirection(0.0f, 0.0f, 0.0f, 0.0f,
                               horizontalHalfFov, verticalHalfFov, direction),
          "aligned target does not generate a spurious edge arrow");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    check(!screenEdgeDirection(nan, 0.0f, 0.0f, 0.0f,
                               horizontalHalfFov, verticalHalfFov, direction),
          "non-finite target angles are rejected");

    const float magnitude = std::hypot(direction.x, direction.y);
    check(std::isfinite(magnitude),
          "invalid input leaves a finite zero direction");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
