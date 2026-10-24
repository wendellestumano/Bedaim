#pragma once

#include <cmath>

namespace bedaim::esplocator {

struct Direction {
    float x = 0.0f;
    float y = 0.0f;
};

inline float wrapDegrees(float angle) noexcept {
    if (!std::isfinite(angle)) return 0.0f;
    float wrapped = std::fmod(angle + 180.0f, 360.0f);
    if (wrapped < 0.0f) wrapped += 360.0f;
    return wrapped - 180.0f;
}

// Produces a unit screen-edge direction. Screen Y is positive downwards.
// Pitch isn't circular: subtracting target and camera pitch preserves whether
// the target is above or below even when the camera is close to +/-90 degrees.
inline bool screenEdgeDirection(float targetYawDegrees, float targetPitchDegrees,
                                float cameraYawDegrees, float cameraPitchDegrees,
                                float halfHorizontalFovDegrees,
                                float halfVerticalFovDegrees,
                                Direction& out) noexcept {
    out = {};
    if (!std::isfinite(targetYawDegrees) || !std::isfinite(targetPitchDegrees) ||
        !std::isfinite(cameraYawDegrees) || !std::isfinite(cameraPitchDegrees) ||
        !std::isfinite(halfHorizontalFovDegrees) || !std::isfinite(halfVerticalFovDegrees) ||
        halfHorizontalFovDegrees <= 0.0f || halfVerticalFovDegrees <= 0.0f) {
        return false;
    }

    const float yawDelta = wrapDegrees(targetYawDegrees - cameraYawDegrees);
    const float pitchDelta = targetPitchDegrees - cameraPitchDegrees;
    const float x = yawDelta / halfHorizontalFovDegrees;
    const float y = pitchDelta / halfVerticalFovDegrees;
    const float length = std::hypot(x, y);
    if (!std::isfinite(length) || length < 1.0e-4f) return false;

    out.x = x / length;
    out.y = y / length;
    return std::isfinite(out.x) && std::isfinite(out.y);
}

} // namespace bedaim::esplocator
