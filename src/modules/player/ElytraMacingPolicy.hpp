#pragma once

#include <algorithm>
#include <cmath>

namespace bedaim::elytramacing {

inline bool finiteDistance(float value) noexcept {
    return std::isfinite(value) && value >= 0.0f;
}

inline float normalizeEngageDistance(float value) noexcept {
    return std::isfinite(value) ? std::clamp(value, 2.0f, 10.0f) : 4.5f;
}

inline bool shouldAimAboveTarget(bool enabled, bool gliding, bool targetValid,
                                 float targetDistance, float engageDistance,
                                 bool chasing) noexcept {
    if (!enabled || !gliding || !targetValid || !chasing ||
        !finiteDistance(targetDistance)) return false;
    return targetDistance > normalizeEngageDistance(engageDistance);
}

inline bool shouldBoostTowardTarget(bool enabled, bool gliding, bool targetValid,
                                    float targetDistance, float engageDistance,
                                    bool cooldownReady) noexcept {
    if (!enabled || !gliding || !targetValid || !cooldownReady ||
        !finiteDistance(targetDistance)) return false;
    return targetDistance > normalizeEngageDistance(engageDistance);
}

inline bool shouldDisengageForMace(bool enabled, bool gliding, bool targetValid,
                                   float targetDistance, float engageDistance,
                                   bool maceAvailable, bool keymapAvailable) noexcept {
    if (!enabled || !gliding || !targetValid || !maceAvailable ||
        !keymapAvailable || !finiteDistance(targetDistance)) return false;
    return targetDistance <= normalizeEngageDistance(engageDistance);
}

inline bool shouldRequestGlide(bool enabled, bool gliding, bool grounded,
                               float verticalDelta, bool keymapAvailable,
                               bool cooldownReady) noexcept {
    // Start the flight sequence while grounded, so the configured keymap can
    // perform the initial jump and second jump. If already airborne, only
    // request recovery during a genuine fall; a level/upward airborne player
    // must not trigger the launch macro at a Wind Burst apex.
    return enabled && !gliding && keymapAvailable && cooldownReady &&
           (grounded || (std::isfinite(verticalDelta) && verticalDelta < -0.025f));
}

inline bool detectedWindBurstRise(bool attackWindowActive, bool gliding,
                                  float verticalDelta, float riseThreshold = 0.08f) noexcept {
    return attackWindowActive && !gliding && std::isfinite(verticalDelta) &&
           std::isfinite(riseThreshold) && verticalDelta >= std::clamp(riseThreshold, 0.02f, 1.0f);
}

inline bool detectedLandingRecovery(bool attackWindowActive, bool gliding, bool sawFall,
                                    int stationaryTicks, int minimumStationaryTicks = 5) noexcept {
    return attackWindowActive && !gliding && sawFall &&
           stationaryTicks >= std::clamp(minimumStationaryTicks, 3, 20);
}

} // namespace bedaim::elytramacing
