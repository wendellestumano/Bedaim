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

// Fail-closed guard for a native wrapper that reports acceptance without any
// immediate positive target hurt-time transition. This is local feedback only,
// not proof of server-side damage. After six consecutive such attempts on the
// same actor, that actor is quarantined until target identity or module state changes.
class NoHitFeedbackGuard {
public:
    static constexpr std::uint32_t kAttemptsBeforeQuarantine = 6;

    bool isBlocked(const void* target) noexcept;
    bool observe(const void* target, bool accepted, int hurtTimeBefore,
                 int hurtTimeAfter) noexcept;
    void reset() noexcept;
    std::uint32_t consecutiveNoFeedback() const noexcept { return m_consecutiveNoFeedback; }

private:
    const void* m_target = nullptr;
    std::uint32_t m_consecutiveNoFeedback = 0;
    bool m_blocked = false;
};

} // namespace bedaim::autohit
