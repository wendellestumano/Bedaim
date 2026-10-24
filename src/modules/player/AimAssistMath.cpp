#include "AimAssistMath.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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

Vec3 aimPointAtHeight(const Bounds& bounds, float yFraction) noexcept {
    const float safeY = std::clamp(std::isfinite(yFraction) ? yFraction : 0.55f,
                                   0.05f, 0.95f);
    return {
        (bounds.min.x + bounds.max.x) * 0.5f,
        bounds.min.y + (bounds.max.y - bounds.min.y) * safeY,
        (bounds.min.z + bounds.max.z) * 0.5f
    };
}

std::array<Vec3, 4> fourAimAnchors(const Bounds& bounds, float yFraction,
                                  float insetFraction) noexcept {
    const float safeInset = std::clamp(std::isfinite(insetFraction) ? insetFraction : 0.25f,
                                       0.05f, 0.45f);
    const Vec3 center = aimPointAtHeight(bounds, yFraction);
    const float xInset = (bounds.max.x - bounds.min.x) * safeInset;
    const float zInset = (bounds.max.z - bounds.min.z) * safeInset;
    return {{
        {bounds.min.x + xInset, center.y, center.z},
        {bounds.max.x - xInset, center.y, center.z},
        {center.x, center.y, bounds.min.z + zInset},
        {center.x, center.y, bounds.max.z - zInset}
    }};
}

std::size_t nearestAnchorToRay(const Vec3& origin, const Vec3& direction,
                               const std::array<Vec3, 4>& anchors) noexcept {
    if (!isFiniteVector(origin) || !isFiniteVector(direction)) return 0;
    const float directionLengthSquared =
        direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
    if (!std::isfinite(directionLengthSquared) || directionLengthSquared <= 1.0e-8f) return 0;

    std::size_t bestIndex = 0;
    float bestDistanceSquared = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        const Vec3& point = anchors[i];
        if (!isFiniteVector(point)) continue;
        const float dx = point.x - origin.x;
        const float dy = point.y - origin.y;
        const float dz = point.z - origin.z;
        const float alongRay = std::max(
            0.0f, (dx * direction.x + dy * direction.y + dz * direction.z) /
                      directionLengthSquared);
        const Vec3 closest{
            origin.x + direction.x * alongRay,
            origin.y + direction.y * alongRay,
            origin.z + direction.z * alongRay
        };
        const float ex = point.x - closest.x;
        const float ey = point.y - closest.y;
        const float ez = point.z - closest.z;
        const float d2 = ex * ex + ey * ey + ez * ez;
        if (std::isfinite(d2) && d2 < bestDistanceSquared) {
            bestDistanceSquared = d2;
            bestIndex = i;
        }
    }
    return bestIndex;
}

float distanceHitboxScale(float baseScale, float distance, float configuredRange,
                          float strength, bool enabled,
                          float minScale, float maxScale) noexcept {
    const float safeBase = std::clamp(
        std::isfinite(baseScale) ? baseScale : 1.0f, 0.30f, 200.0f);
    if (!enabled) return safeBase;

    const float safeMinimum = std::clamp(
        std::isfinite(minScale) ? minScale : 0.25f, 0.25f, 200.0f);
    const float safeMaximum = std::clamp(
        std::isfinite(maxScale) ? maxScale : 200.0f, safeMinimum, 200.0f);

    const float safeDistance = std::max(0.0f,
        std::isfinite(distance) ? distance : 0.0f);
    const float safeRange = std::clamp(
        std::isfinite(configuredRange) ? configuredRange : 5.5f, 2.0f, 80.0f);
    const float safeStrength = std::clamp(
        std::isfinite(strength) ? strength : 5.0f, 0.1f, 500.0f);

    // Keep the steering volume deliberately tight through close-range combat.
    // Stretch the curve over the user's real scan range, but cap it at 30 blocks
    // so the strength slider still has a visible effect at ordinary distances.
    constexpr float kNearDistance = 1.75f;

    // Stretch the near-to-far curve over the configured aim range (up to 30 blocks).
    // This matters on normal 4-6 block settings too: far targets should receive the
    // configured strength instead of the slider being muted by an arbitrary 8-block floor.
    constexpr float kMaximumCurveRange = 30.0f;
    const float farDistance = std::clamp(safeRange, 2.5f, kMaximumCurveRange);
    const float linearWithinRange = std::clamp(
        (safeDistance - kNearDistance) / std::max(1.0f, farDistance - kNearDistance),
        0.0f, 1.0f);
    const float distanceBlendWithinRange = linearWithinRange * linearWithinRange *
        (3.0f - 2.0f * linearWithinRange);
    // Strength=5 gives a 2.8x far multiplier. Greater strength is more pronounced,
    // still bounded; the close-range multiplier is deliberately below one.
    const float strengthFactor = std::sqrt(safeStrength / 5.0f);
    const float farMultiplier = std::clamp(0.8f + 2.0f * strengthFactor, 1.1f, 40.0f);
    const float multiplier = 0.4f + (farMultiplier - 0.4f) * distanceBlendWithinRange;
    // Clamp the result, not the strength slider: these controls cap the actual
    // virtual hitbox scale produced by the distance curve. Native collision is unchanged.
    return std::clamp(safeBase * multiplier, safeMinimum, safeMaximum);
}

} // namespace bedaim::aimmath
