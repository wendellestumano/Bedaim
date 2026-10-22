#pragma once

#include <array>
#include <cstddef>

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

// Return the desired point at a normalized height fraction. X/Z remain centered.
Vec3 aimPointAtHeight(const Bounds& bounds, float yFraction) noexcept;
// Four cardinal anchors inside the real hitbox, all at the requested Y fraction.
std::array<Vec3, 4> fourAimAnchors(const Bounds& bounds, float yFraction,
                                  float insetFraction = 0.25f) noexcept;
// Select the anchor with the smallest distance to the forward view ray.
std::size_t nearestAnchorToRay(const Vec3& origin, const Vec3& direction,
                               const std::array<Vec3, 4>& anchors) noexcept;

// Scale for the virtual steering area: smaller than the native box nearby,
// then progressively larger with distance. This does not modify native collision.
float distanceHitboxScale(float baseScale, float distance, float configuredRange,
                          float strength, bool enabled) noexcept;
} // namespace bedaim::aimmath
