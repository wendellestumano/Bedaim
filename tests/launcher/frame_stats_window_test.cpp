#include "modules/hud/FrameStatsWindow.hpp"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using bedaim::hud::FrameStatsWindow;
    FrameStatsWindow stats(1000);
    assert(stats.snapshot().sampleCount == 0);

    stats.addSample(16, 16.0f);
    stats.addSample(32, 16.0f);
    stats.addSample(64, 32.0f);
    auto snapshot = stats.snapshot();
    assert(snapshot.sampleCount == 3);
    assert(std::abs(snapshot.currentFps - 31.25f) < 0.01f);
    assert(std::abs(snapshot.averageFrameMs - (64.0f / 3.0f)) < 0.01f);
    assert(std::abs(snapshot.averageFps - 46.875f) < 0.02f);
    assert(std::abs(snapshot.onePercentLowFps - 31.25f) < 0.01f);
    assert(std::abs(snapshot.worstFrameMs - 32.0f) < 0.01f);

    stats.addSample(70, 0.0f);
    stats.addSample(71, -2.0f);
    stats.addSample(72, std::numeric_limits<float>::infinity());
    stats.addSample(73, 1200.0f);
    assert(stats.snapshot().sampleCount == 3);

    stats.addSample(1100, 10.0f);
    snapshot = stats.snapshot();
    assert(snapshot.sampleCount == 2);
    assert(std::abs(snapshot.averageFrameMs - 21.0f) < 0.01f);

    stats.addSample(500, 20.0f);
    snapshot = stats.snapshot();
    assert(snapshot.sampleCount == 1);
    assert(std::abs(snapshot.currentFps - 50.0f) < 0.01f);

    stats.reset();
    assert(stats.snapshot().sampleCount == 0);
    return 0;
}
