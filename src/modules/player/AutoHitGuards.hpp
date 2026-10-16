#pragma once

namespace bedaim::autohit {

// Policy boundary for native attack dispatch. The entry must match the exact
// public GameMode/SurvivalMode signature resolved from the active runtime.
bool publicAttackRouteAvailable(bool publicEntryMatchesResolvedSignature) noexcept;

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

// Extends the native actor query far enough to include AutoHit's configured reach.
float effectiveScanRange(float aimAssistRange, bool autoHitEnabled, float autoHitRange,
                         float queryExpansionMargin = 0.0f) noexcept;

} // namespace bedaim::autohit
