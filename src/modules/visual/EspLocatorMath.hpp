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
                        float aspect, float verticalFov = 110.0f) noexcept {
    if (!finite(localPosition) || !finite(targetCenter) ||
        !std::isfinite(rotation.pitch) || !std::isfinite(rotation.yaw)) return false;

    const float dx = targetCenter.x - localPosition.x;
    const float dy = targetCenter.y - localPosition.y;
    const float dz = targetCenter.z - localPosition.z;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) return false;

    // Bedrock camera basis shared with edgeDirection. This is the same idea as
    // Apollon's view-matrix projection: test camera-space depth before the
    // perspective divide. In particular, a target behind the camera is always
    // off-screen rather than being accidentally accepted by wrapped yaw angles.
    const float yaw = rotation.yaw * kDegToRad;
    const float pitch = rotation.pitch * kDegToRad;
    const float cosPitch = std::cos(pitch);
    const float sinPitch = std::sin(pitch);
    const float cosYaw = std::cos(yaw);
    const float sinYaw = std::sin(yaw);
    const float rightX = -cosYaw;
    const float rightZ = -sinYaw;
    const float upX = -sinPitch * sinYaw;
    const float upY = cosPitch;
    const float upZ = sinPitch * cosYaw;
    const float forwardX = -sinYaw * cosPitch;
    const float forwardY = -sinPitch;
    const float forwardZ = cosYaw * cosPitch;

    const float cameraDepth = dx * forwardX + dy * forwardY + dz * forwardZ;
    const float cameraRight = dx * rightX + dz * rightZ;
    const float cameraUp = dx * upX + dy * upY + dz * upZ;
    if (!std::isfinite(cameraDepth) || !std::isfinite(cameraRight) ||
        !std::isfinite(cameraUp)) return false;
    if (cameraDepth <= 0.001f) return true;

    const float safeAspect = std::clamp(std::isfinite(aspect) ? aspect : (16.0f / 9.0f),
                                        0.5f, 3.2f);
    const float safeFov = std::clamp(std::isfinite(verticalFov) ? verticalFov : 110.0f,
                                     30.0f, 150.0f);
    const float halfVertical = safeFov * 0.5f + 1.5f;
    const float halfHorizontal =
        std::atan(std::tan(safeFov * 0.5f * kDegToRad) * safeAspect) * kRadToDeg + 2.0f;
    const float yawAngle = std::atan2(cameraRight, cameraDepth) * kRadToDeg;
    const float pitchAngle = std::atan2(cameraUp, cameraDepth) * kRadToDeg;
    return std::fabs(yawAngle) > halfHorizontal || std::fabs(pitchAngle) > halfVertical;
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