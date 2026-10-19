#pragma once

#include <cstdint>

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

// Extends the native actor query far enough to include AutoHit's configured reach.
float effectiveScanRange(float aimAssistRange, bool autoHitEnabled, float autoHitRange,
                         float queryExpansionMargin = 0.0f) noexcept;

// Requires the engine's current HitResult to identify the exact selected target.
// A missing target or mismatched raycast result must never dispatch an attack.
bool hitResultMatchesTarget(const void* target, const void* hitResultTarget) noexcept;

// Diagnostic-only tracker for a native call that reports acceptance without
// an immediate local hurt-time transition. Hurt-time can update after the current
// tick, so this must never gate attack dispatch. observe() returns true once when
// the streak reaches the threshold, allowing one warning while the configured CPS
// limiter continues to bound attempts.
class NoHitFeedbackGuard {
public:
    static constexpr std::uint32_t kAttemptsBeforeWarning = 6;

    bool observe(const void* target, bool accepted, int hurtTimeBefore,
                 int hurtTimeAfter) noexcept;
    void reset() noexcept;
    std::uint32_t consecutiveNoFeedback() const noexcept { return m_consecutiveNoFeedback; }

private:
    const void* m_target = nullptr;
    std::uint32_t m_consecutiveNoFeedback = 0;
    bool m_warningEmitted = false;
};

} // namespace bedaim::autohit
