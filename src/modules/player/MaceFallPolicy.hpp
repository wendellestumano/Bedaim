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

// A bounded burst continues only while presses remain; an exhausted burst stays latched.
inline bool shouldDispatchUnglideBurst(bool alreadyTriggered, int burstRemaining) noexcept {
    return !alreadyTriggered || burstRemaining > 0;
}

inline bool shouldBeginUnglideDispatch(bool enabled, bool dispatchInProgress,
                                       bool alreadyTriggered, int burstRemaining) noexcept {
    return enabled && !dispatchInProgress && burstRemaining >= 0 &&
           shouldDispatchUnglideBurst(alreadyTriggered, burstRemaining);
}

// A completed trigger stays latched through transient gate failures. Only a
// lost/changed target or explicit separation beyond the re-arm radius may reset it.
inline bool shouldKeepUnglideTriggerLatch(bool wasTriggered, bool targetLost,
                                          bool targetChanged, bool outsideRearmRadius) noexcept {
    return wasTriggered && !targetLost && !targetChanged && !outsideRearmRadius;
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