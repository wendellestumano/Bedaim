#include "modules/hud/SpeedDisplayFormat.hpp"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using bedaim::hud::formatSpeed;

    assert(formatSpeed(0.0f) == "Speed: 0 m/s");
    assert(formatSpeed(1.236f, 2) == "Speed: 1.24 m/s");
    assert(formatSpeed(1.26f, 1) == "Speed: 1.3 m/s");
    assert(formatSpeed(1.9f, 0) == "Speed: 2 m/s");
    assert(formatSpeed(1.234f, -5) == "Speed: 1 m/s");
    assert(formatSpeed(1.234f, 5) == "Speed: 1.23 m/s");
    assert(formatSpeed(-1.0f, 2) == "Speed: 0.00 m/s");
    assert(formatSpeed(std::numeric_limits<float>::infinity(), 1) == "Speed: 0.0 m/s");
    assert(formatSpeed(std::numeric_limits<float>::quiet_NaN(), 1) == "Speed: 0.0 m/s");
    return 0;
}
