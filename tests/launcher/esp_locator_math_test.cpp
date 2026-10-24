#include "modules/visual/EspLocatorMath.hpp"
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string_view>

namespace {
int checks=0, failures=0;
void check(bool ok, std::string_view label) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
}

int main() {
    using namespace bedaim::esp_locator;
    Direction dir{};
    const Vec3 origin{0, 0, 0};
    const Rotation level{0, 0};
    check(edgeDirection(origin, {10, 0, 10}, level, dir) && dir.x < 0,
          "target +X is left when Bedrock yaw=0 faces +Z");
    check(edgeDirection(origin, {-10, 0, 10}, level, dir) && dir.x > 0,
          "target -X is right when Bedrock yaw=0 faces +Z");
    check(edgeDirection(origin, {0, 10, 10}, level, dir) && dir.y < 0,
          "target above the player points toward the top edge");
    check(edgeDirection(origin, {0, -10, 10}, level, dir) && dir.y > 0,
          "target below the player points toward the bottom edge");
    check(edgeDirection(origin, {0, 0, -10}, level, dir) &&
              std::isfinite(dir.x) && std::isfinite(dir.y) &&
              std::abs(std::hypot(dir.x,dir.y)-1.0f) < 1e-4f,
          "directly behind target has a finite stable edge direction");

    check(!isOffscreen(origin, {0, 0, 10}, level, 16.0f/9.0f),
          "target straight ahead is not classified as off-screen");
    check(isOffscreen(origin, {0, 10, 0}, level, 16.0f/9.0f),
          "near-vertical target is not discarded by a horizontal-distance gate");
    check(edgeDirection(origin, {0, 10, 0}, level, dir) && dir.y < 0,
          "exactly vertical target still maps to a vertical edge");

    const float upPitch = -80.0f;
    const float p = upPitch * kDegToRad;
    const Vec3 viewAligned{0.0f, -std::sin(p)*10.0f, std::cos(p)*10.0f};
    check(!isOffscreen(origin, viewAligned, {upPitch,0}, 16.0f/9.0f),
          "target aligned with steep upward view stays in view");
    check(isOffscreen(origin, {0,0,10}, {upPitch,0}, 16.0f/9.0f) &&
              edgeDirection(origin, {0,0,10}, {upPitch,0}, dir) && dir.y > 0,
          "level target appears below the screen while looking steeply upward");

    check(isOffscreen(origin, {0,0,10}, {80.0f,0}, 16.0f/9.0f) &&
              edgeDirection(origin, {0,0,10}, {80.0f,0}, dir) && dir.y < 0,
          "level target appears above the screen while looking steeply downward");
    check(!isOffscreen(origin, {0,0,10}, {0, 0}, std::nanf("")),
          "non-finite viewport aspect uses a safe default");
    check(!edgeDirection(origin, {0,0,0}, level, dir),
          "zero-length target vector is rejected without division by zero");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}