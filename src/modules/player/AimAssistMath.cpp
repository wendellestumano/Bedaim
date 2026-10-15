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

float distanceHitboxScale(float baseScale, float distance, float configuredRange,
                          float strength, bool enabled) noexcept {
    const float safeBase = std::clamp(
        std::isfinite(baseScale) ? baseScale : 1.0f, 1.0f, 200.0f);
    if (!enabled) return safeBase;

    const float safeDistance = std::max(0.0f,
        std::isfinite(distance) ? distance : 0.0f);
    const float safeRange = std::clamp(
        std::isfinite(configuredRange) ? configuredRange : 5.5f, 2.0f, 80.0f);
    const float safeStrength = std::clamp(
        std::isfinite(strength) ? strength : 5.0f, 0.1f, 200.0f);

    // Keep the steering volume deliberately tight through close-range combat.
    // Stretch the curve over the user's real scan range, but cap it at 30 blocks
    // so the strength slider still has a visible effect at ordinary distances.
    constexpr float kNearDistance = 1.75f;
    const float farDistance = std::clamp(safeRange, 8.0f, 30.0f);
    const float linear = std::clamp(
        (safeDistance - kNearDistance) / std::max(1.0f, farDistance - kNearDistance),
        0.0f, 1.0f);
    const float distanceBlend = linear * linear * (3.0f - 2.0f * linear);

    // Strength=5 keeps a useful ~2.8x far-range multiplier. Larger values have a
    // stronger, but bounded, effect; unlike the previous curve this also permits
    // a sub-native virtual area near the target instead of clamping it to 1.0.
    const float strengthFactor = std::sqrt(safeStrength / 5.0f);
    const float farMultiplier = std::clamp(0.8f + 2.0f * strengthFactor, 1.1f, 12.0f);
    const float multiplier = 0.4f + (farMultiplier - 0.4f) * distanceBlend;
    return std::clamp(safeBase * multiplier, 0.25f, 200.0f);
}

} // namespace bedaim::aimmath
