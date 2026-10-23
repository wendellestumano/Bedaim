#pragma once

#include <algorithm>
#include <cmath>

namespace bedaim::macefall {
inline float normalizeMinimumFallDistance(float distance) noexcept {
    return std::isfinite(distance) ? std::clamp(distance, 0.30f, 10.0f) : 1.55f;
}
inline bool shouldBeginMaceAction(float verticalDelta, float fallDistance,
                                  float minimumFallDistance) noexcept {
    if (!std::isfinite(verticalDelta) || !std::isfinite(fallDistance) || fallDistance < 0.0f)
        return false;
    return verticalDelta < -0.025f &&
           fallDistance >= normalizeMinimumFallDistance(minimumFallDistance);
}

inline bool shouldPulseUnglideJump(bool enabled, bool isGliding,
                                    float targetDistance, float maximumTargetDistance) noexcept {
    if (!enabled || !isGliding ||
        !std::isfinite(targetDistance) || !std::isfinite(maximumTargetDistance) ||
        targetDistance < 0.0f) {
        return false;
    }
    const float safeTargetDistance = std::clamp(maximumTargetDistance, 1.0f, 32.0f);
    return targetDistance <= safeTargetDistance;
}

// A target can start one bounded burst. Once that burst is exhausted, the
// same still-valid target must not start another burst until state is reset.
inline bool shouldDispatchUnglideBurst(bool alreadyTriggered, int burstRemaining) noexcept {
    return !alreadyTriggered || burstRemaining > 0;
}

inline bool shouldTriggerUnglideKeymap(bool enabled, bool targetIsPlayer,
                                       bool crosshairConfirmed, float targetDistance,
                                       float maximumTargetDistance) noexcept {
    if (!enabled || !targetIsPlayer || !crosshairConfirmed ||
        !std::isfinite(targetDistance) || !std::isfinite(maximumTargetDistance) ||
        targetDistance < 0.0f) {
        return false;
    }
    const float safeTargetDistance = std::clamp(maximumTargetDistance, 1.0f, 32.0f);
    return targetDistance <= safeTargetDistance;
}
} // namespace bedaim::macefall
