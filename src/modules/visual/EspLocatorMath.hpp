#pragma once
#include <algorithm>
#include <cmath>

namespace bedaim::esp_locator {
struct Vec3 { float x, y, z; };
struct Rotation { float pitch, yaw; };
struct Direction { float x, y; };

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kDegToRad = kPi / 180.0f;

inline bool finite(Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
inline float wrapDegrees(float value) noexcept {
    if (!std::isfinite(value)) return 0.0f;
    value = std::fmod(value + 180.0f, 360.0f);
    if (value < 0.0f) value += 360.0f;
    return value - 180.0f;
}
inline bool relativeAngles(Vec3 localPosition, Vec3 targetCenter, Rotation rotation,
                           float& yawDelta, float& pitchDelta) noexcept {
    if (!finite(localPosition) || !finite(targetCenter) ||
        !std::isfinite(rotation.pitch) || !std::isfinite(rotation.yaw)) return false;
    const float dx = targetCenter.x - localPosition.x;
    const float dy = targetCenter.y - localPosition.y;
    const float dz = targetCenter.z - localPosition.z;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) return false;
    const float horizontal = std::hypot(dx, dz);
    if (!std::isfinite(horizontal)) return false;
    // Yaw is undefined for an exactly vertical target. Use the current yaw so
    // that target remains vertically aligned instead of producing a fake side bearing.
    const float targetYaw = horizontal < 0.001f
        ? rotation.yaw : std::atan2(-dx, dz) * kRadToDeg;
    const float targetPitch = -std::atan2(dy, horizontal) * kRadToDeg;
    yawDelta = wrapDegrees(targetYaw - rotation.yaw);
    pitchDelta = targetPitch - rotation.pitch;
    return std::isfinite(yawDelta) && std::isfinite(pitchDelta);
}
inline bool isOffscreen(Vec3 localPosition, Vec3 targetCenter, Rotation rotation,
                        float aspect) noexcept {
    float yawDelta = 0.0f, pitchDelta = 0.0f;
    if (!relativeAngles(localPosition, targetCenter, rotation, yawDelta, pitchDelta)) return false;
    constexpr float verticalFov = 110.0f;
    const float safeAspect = std::clamp(std::isfinite(aspect) ? aspect : (16.0f / 9.0f),
                                        0.5f, 3.2f);
    const float halfVertical = verticalFov * 0.5f + 1.5f;
    const float halfHorizontal =
        std::atan(std::tan(verticalFov * 0.5f * kDegToRad) * safeAspect) * kRadToDeg + 2.0f;
    return std::fabs(yawDelta) > halfHorizontal || std::fabs(pitchDelta) > halfVertical;
}
inline bool edgeDirection(Vec3 localPosition, Vec3 targetCenter, Rotation rotation,
                          Direction& out) noexcept {
    out = {0.0f, 0.0f};
    if (!finite(localPosition) || !finite(targetCenter) ||
        !std::isfinite(rotation.pitch) || !std::isfinite(rotation.yaw)) return false;

    const float dx0 = targetCenter.x - localPosition.x;
    const float dy0 = targetCenter.y - localPosition.y;
    const float dz0 = targetCenter.z - localPosition.z;
    const float worldLength = std::sqrt(dx0 * dx0 + dy0 * dy0 + dz0 * dz0);
    if (!std::isfinite(worldLength) || worldLength < 0.001f) return false;
    const float dx = dx0 / worldLength;
    const float dy = dy0 / worldLength;
    const float dz = dz0 / worldLength;

    const float yaw = rotation.yaw * kDegToRad;
    const float pitch = rotation.pitch * kDegToRad;
    const float cosPitch = std::cos(pitch);
    const float sinPitch = std::sin(pitch);
    const float cosYaw = std::cos(yaw);
    const float sinYaw = std::sin(yaw);

    // Bedrock forward is (-sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch)).
    // Bedrock yaw=0 faces +Z (south), so screen-right points toward -X.
    // Keep this basis consistent with the forward convention used by AimAssist.
    const float rightX = -cosYaw;
    const float rightZ = -sinYaw;
    const float upX = -sinPitch * sinYaw;
    const float upY = cosPitch;
    const float upZ = sinPitch * cosYaw;
    const float forwardX = -sinYaw * cosPitch;
    const float forwardY = -sinPitch;
    const float forwardZ = cosYaw * cosPitch;

    const float cameraDepth = dx * forwardX + dy * forwardY + dz * forwardZ;
    float dirX = dx * rightX + dz * rightZ;
    float dirY = -(dx * upX + dy * upY + dz * upZ);
    if (cameraDepth < 0.0f) { dirX = -dirX; dirY = -dirY; }

    float length = std::hypot(dirX, dirY);
    if (length < 0.0001f) {
        // Degenerate projection (e.g. exactly behind the camera): use the angular
        // deltas as a stable fallback instead of always forcing the arrow downward.
        float yawDelta = 0.0f, pitchDelta = 0.0f;
        if (!relativeAngles(localPosition, targetCenter, rotation, yawDelta, pitchDelta)) return false;
        dirX = std::sin(yawDelta * kDegToRad);
        dirY = std::sin(pitchDelta * kDegToRad);
        if (cameraDepth < 0.0f) { dirX = -dirX; dirY = -dirY; }
        length = std::hypot(dirX, dirY);
        if (length < 0.0001f) {
            if (std::fabs(yawDelta) > 90.0f) {
                dirX = yawDelta >= 0.0f ? 1.0f : -1.0f;
                dirY = 0.0f;
            } else {
                dirX = 0.0f;
                dirY = pitchDelta >= 0.0f ? 1.0f : -1.0f;
            }
            length = 1.0f;
        }
    }
    out = {dirX / length, dirY / length};
    return std::isfinite(out.x) && std::isfinite(out.y);
}
} // namespace bedaim::esp_locator