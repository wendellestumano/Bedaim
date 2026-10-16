#include "modules/hud/DistanceAccumulator.hpp"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using bedaim::hud::DistanceAccumulator;
    DistanceAccumulator distance;
    assert(distance.totalBlocks() == 0.0);

    distance.update(0.0f, 0.0f, 0.0f, false, 16.0f); // baseline only
    distance.update(3.0f, 99.0f, 4.0f, false, 16.0f);
    assert(std::abs(distance.totalBlocks() - 5.0) < 0.001);

    distance.update(3.0f, 111.0f, 4.0f, true, 16.0f);
    assert(std::abs(distance.totalBlocks() - 17.0) < 0.001);

    distance.update(100.0f, 111.0f, 4.0f, true, 16.0f); // teleport is ignored
    assert(std::abs(distance.totalBlocks() - 17.0) < 0.001);
    distance.update(103.0f, 111.0f, 8.0f, true, 16.0f);
    assert(std::abs(distance.totalBlocks() - 22.0) < 0.001);

    distance.update(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, true, 16.0f);
    distance.update(0.0f, 0.0f, 0.0f, true, 16.0f); // invalid point resets baseline
    distance.update(1.0f, 0.0f, 0.0f, true, 16.0f);
    assert(std::abs(distance.totalBlocks() - 23.0) < 0.001);

    distance.reset();
    assert(distance.totalBlocks() == 0.0);
    distance.update(0.0f, 0.0f, 0.0f, false, 0.0f);
    distance.update(3.0f, 0.0f, 4.0f, false, 0.0f); // threshold clamps to 1; reject step
    assert(distance.totalBlocks() == 0.0);
    return 0;
}
