#include "AimAssistMath.hpp"

#include <algorithm>
#include <cmath>

namespace bedaim::aimmath {

bool isFiniteVector(const Vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool isValidBounds(const Bounds& bounds) noexcept {
    if (!isFiniteVector(bounds.min) || !isFiniteVector(bounds.max)) return false;

    const float height = bounds.max.y - bounds.min.y;
    const float width = bounds.max.x - bounds.min.x;
    const float depth = bounds.max.z - bounds.min.z;
    return height >= 0.25f && height <= 8.0f &&
           width > 0.0f && width <= 5.0f &&
           depth > 0.0f && depth <= 5.0f;
}

float wrapDegrees(float angle) noexcept {
    if (!std::isfinite(angle)) return 0.0f;
    angle = std::fmod(angle, 360.0f);
    if (angle > 180.0f) angle -= 360.0f;
    else if (angle < -180.0f) angle += 360.0f;
    return angle;
}

float boundedTurnStep(float angularErrorDegrees,
                      float turnRateDegreesPerSecond,
                      float deltaSeconds) noexcept {
    if (!std::isfinite(angularErrorDegrees) ||
        !std::isfinite(turnRateDegreesPerSecond) ||
        !std::isfinite(deltaSeconds) ||
        turnRateDegreesPerSecond <= 0.0f ||
        deltaSeconds <= 0.0f) {
        return 0.0f;
    }

    const float safeDelta = std::clamp(deltaSeconds, 0.002f, 0.05f);
    const float maximumStep = turnRateDegreesPerSecond * safeDelta;
    if (!std::isfinite(maximumStep) || maximumStep <= 0.0f) return 0.0f;
    return std::clamp(angularErrorDegrees, -maximumStep, maximumStep);
}

} // namespace bedaim::aimmath
