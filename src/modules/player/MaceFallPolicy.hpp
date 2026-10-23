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

inline bool shouldRearmUnglide(bool currentlyArmed, bool targetChanged,
                               float targetDistance, float triggerDistance,
                               float rearmMargin) noexcept {
    if (currentlyArmed || targetChanged) return true;
    if (!std::isfinite(targetDistance) || !std::isfinite(triggerDistance) ||
        !std::isfinite(rearmMargin) || targetDistance < 0.0f) return false;
    const float safeTriggerDistance = std::clamp(triggerDistance, 1.0f, 32.0f);
    const float safeMargin = std::clamp(rearmMargin, 0.5f, 8.0f);
    return targetDistance > safeTriggerDistance + safeMargin;
}
} // namespace bedaim::macefall
