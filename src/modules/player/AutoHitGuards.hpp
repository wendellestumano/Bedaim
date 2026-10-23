#pragma once

namespace bedaim::autohit {

enum class Decision {
    Ready,
    Disabled,
    MissingPlayer,
    MissingTarget,
    InvalidBounds,
    StaleTarget,
    InvalidHitPoint,
    OutOfRange,
    Cooldown
};

struct Inputs {
    bool enabled;
    bool localPlayerValid;
    bool targetPresent;
    bool boundsValid;
    bool targetFresh;
    bool hitPointFinite;
    bool cooldownReady;
    float distance;
    float maxDistance;
};

// Key Mapper dispatch gate. It evaluates target, geometry, range and cooldown
// facts already gathered by the caller; it never dispatches a game hit.
Decision evaluate(const Inputs& input) noexcept;

// Extends the Aim Assist target query far enough to include AutoHit's configured reach.
float effectiveScanRange(float aimAssistRange, bool autoHitEnabled, float autoHitRange,
                         float queryExpansionMargin = 0.0f) noexcept;

} // namespace bedaim::autohit
