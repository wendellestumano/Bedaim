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

// A bounded burst continues only while presses remain; an exhausted burst stays latched.
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

inline bool shouldRearmUnglide(float targetDistance, float triggerDistance,
                               float hysteresis = 1.5f) noexcept {
    if (!std::isfinite(targetDistance) || !std::isfinite(triggerDistance) ||
        !std::isfinite(hysteresis) || targetDistance < 0.0f ||
        triggerDistance < 1.0f || hysteresis < 0.0f) return false;
    return targetDistance > std::clamp(triggerDistance, 1.0f, 32.0f) + hysteresis;
}
} // namespace bedaim::macefall
