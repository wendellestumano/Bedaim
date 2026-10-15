#pragma once

namespace bedaim::aimmath {

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Bounds {
    Vec3 min;
    Vec3 max;
};

bool isFiniteVector(const Vec3& value) noexcept;
bool isValidBounds(const Bounds& bounds) noexcept;
float wrapDegrees(float angle) noexcept;
float boundedTurnStep(float angularErrorDegrees,
                      float turnRateDegreesPerSecond,
                      float deltaSeconds) noexcept;

// Scale for the virtual steering area: smaller than the native box nearby,
// then progressively larger with distance. This does not modify native collision.
float distanceHitboxScale(float baseScale, float distance, float configuredRange,
                          float strength, bool enabled) noexcept;

} // namespace bedaim::aimmath
