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
Vec3 selectAimPoint(bool stickyTracking,
                     const Vec3& centerPoint,
                     const Vec3& slidingPoint) noexcept;

} // namespace bedaim::aimmath
