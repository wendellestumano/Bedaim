#include "esp_geometry.hpp"

#include <algorithm>
#include <cmath>

namespace bedaim::esp_geometry {
namespace {
constexpr double kTau = 6.283185307179586476925286766559;

bool finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3 pointAt(const Vec3& center,
             const Vec3& right,
             const Vec3& up,
             const Vec3& normal,
             float radius,
             float angle) {
    const float c = std::cos(angle) * radius;
    const float s = std::sin(angle) * radius;
    return {
        center.x + normal.x * c + up.x * s,
        center.y + normal.y * c + up.y * s,
        center.z + normal.z * c + up.z * s
    };
}
} // namespace

bool isValidBounds(const AABB& b) {
    return finite(b.min) && finite(b.max) &&
           b.max.x > b.min.x && b.max.y > b.min.y && b.max.z > b.min.z;
}

bool buildAabbEdges(const AABB& b, AabbEdges& out) {
    if (!isValidBounds(b)) return false;

    const Vec3 mn = b.min;
    const Vec3 mx = b.max;
    out = {{
        {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}},
        {{mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}},
        {{mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z}},
        {{mn.x, mn.y, mx.z}, {mn.x, mn.y, mn.z}},

        {{mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}},
        {{mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}},
        {{mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}},
        {{mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z}},

        {{mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}},
        {{mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}},
        {{mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}},
        {{mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}}
    }};
    return true;
}

bool buildTargetRing(const AABB& b,
                     const Vec3& camera,
                     bool animated,
                     float indicatorSize,
                     double elapsedSeconds,
                     TargetRingGeometry& output) {
    if (!isValidBounds(b) || !finite(camera) || !std::isfinite(elapsedSeconds)) return false;

    const float width = b.max.x - b.min.x;
    const float height = b.max.y - b.min.y;
    const Vec3 center{
        (b.min.x + b.max.x) * 0.5f,
        b.max.y + std::max(0.18f, height * 0.14f),
        (b.min.z + b.max.z) * 0.5f
    };

    float nx = camera.x - center.x;
    float ny = camera.y - center.y;
    float nz = camera.z - center.z;
    const float nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!std::isfinite(nLen) || nLen < 0.001f) return false;
    nx /= nLen;
    ny /= nLen;
    nz /= nLen;

    // Camera-facing local basis. Fall back to world X if the view direction is vertical.
    float rx = nz;
    float ry = 0.0f;
    float rz = -nx;
    const float rLen = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rLen < 0.001f) {
        rx = 1.0f;
        ry = 0.0f;
        rz = 0.0f;
    } else {
        rx /= rLen;
        ry /= rLen;
        rz /= rLen;
    }
    const Vec3 right{rx, ry, rz};
    const Vec3 normal{nx, ny, nz};
    const Vec3 up{
        ny * rz - nz * ry,
        nz * rx - nx * rz,
        nx * ry - ny * rx
    };

    const double animationRate = animated ? 1.0 : 0.0;
    const float outerPhase = static_cast<float>(std::fmod(elapsedSeconds * 0.78 * animationRate, kTau));
    const float innerPhase = static_cast<float>(std::fmod(-elapsedSeconds * 1.12 * animationRate, kTau));
    const float sizeScale = std::clamp(indicatorSize / 48.0f, 0.5f, 2.0f);
    const float radius = std::clamp(std::max(width * 1.02f, 0.42f) * sizeScale, 0.22f, 1.20f);

    std::size_t outerIndex = 0;
    constexpr int kOuterArcSteps = 64;
    for (int i = 0; i < kOuterArcSteps; ++i) {
        if ((i % 16) >= 12) continue;
        const float a0 = static_cast<float>(kTau * i / kOuterArcSteps) + outerPhase;
        const float a1 = static_cast<float>(kTau * (i + 1) / kOuterArcSteps) + outerPhase;
        output.outer[outerIndex++] = {
            pointAt(center, right, up, normal, radius, a0),
            pointAt(center, right, up, normal, radius, a1)
        };
    }

    for (int i = 0; i < 4; ++i) {
        const float angle = static_cast<float>(kTau * i / 4.0) + outerPhase;
        const Vec3 tip = pointAt(center, right, up, normal, radius * 0.78f, angle);
        const Vec3 base = pointAt(center, right, up, normal, radius * 1.18f, angle);
        const float wing = radius * 0.18f;
        const Vec3 tangent{
            -right.x * std::sin(angle) + up.x * std::cos(angle),
            -right.y * std::sin(angle) + up.y * std::cos(angle),
            -right.z * std::sin(angle) + up.z * std::cos(angle)
        };
        const Vec3 wingA{base.x + tangent.x * wing, base.y + tangent.y * wing, base.z + tangent.z * wing};
        const Vec3 wingB{base.x - tangent.x * wing, base.y - tangent.y * wing, base.z - tangent.z * wing};
        output.outer[outerIndex++] = {tip, wingA};
        output.outer[outerIndex++] = {wingA, wingB};
        output.outer[outerIndex++] = {wingB, tip};
    }

    std::size_t innerIndex = 0;
    constexpr int kMiddleArcSteps = 48;
    for (int i = 0; i < kMiddleArcSteps; ++i) {
        if ((i % 12) >= 10) continue;
        const float a0 = static_cast<float>(kTau * i / kMiddleArcSteps) + innerPhase;
        const float a1 = static_cast<float>(kTau * (i + 1) / kMiddleArcSteps) + innerPhase;
        output.inner[innerIndex++] = {
            pointAt(center, right, up, normal, radius * 0.70f, a0),
            pointAt(center, right, up, normal, radius * 0.70f, a1)
        };
    }

    constexpr int kInnerArcSteps = 32;
    for (int i = 0; i < kInnerArcSteps; ++i) {
        if ((i % 8) == 7) continue;
        const float a0 = static_cast<float>(kTau * i / kInnerArcSteps) - outerPhase * 0.45f;
        const float a1 = static_cast<float>(kTau * (i + 1) / kInnerArcSteps) - outerPhase * 0.45f;
        output.inner[innerIndex++] = {
            pointAt(center, right, up, normal, radius * 0.39f, a0),
            pointAt(center, right, up, normal, radius * 0.39f, a1)
        };
    }

    if (outerIndex != output.outer.size() || innerIndex != output.inner.size()) return false;
    for (const auto& segment : output.outer) {
        if (!finite(segment.first) || !finite(segment.second)) return false;
    }
    for (const auto& segment : output.inner) {
        if (!finite(segment.first) || !finite(segment.second)) return false;
    }
    return true;
}

} // namespace bedaim::esp_geometry
