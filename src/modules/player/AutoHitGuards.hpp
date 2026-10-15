#pragma once

namespace bedaim::autohit {

enum class Decision {
    Ready,
    Disabled,
    MissingPlayer,
    MissingTarget,
    InvalidBounds,
    StaleTarget,
    MissingGameMode,
    MissingAttackRoute,
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
    bool gameModeValid;
    bool attackRouteAvailable;
    bool hitPointFinite;
    bool cooldownReady;
    float distance;
    float maxDistance;
};

// Native-free final dispatch gate. It only evaluates facts already gathered by
// the caller; it never reads game memory or calls the engine.
Decision evaluate(const Inputs& input) noexcept;

} // namespace bedaim::autohit
