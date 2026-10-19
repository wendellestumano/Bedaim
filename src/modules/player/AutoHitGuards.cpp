#include "AutoHitGuards.hpp"

#include <algorithm>
#include <cmath>

namespace bedaim::autohit {

Decision evaluate(const Inputs& input) noexcept {
    if (!input.enabled) return Decision::Disabled;
    if (!input.localPlayerValid) return Decision::MissingPlayer;
    if (!input.targetPresent) return Decision::MissingTarget;
    if (!input.boundsValid) return Decision::InvalidBounds;
    if (!input.targetFresh) return Decision::StaleTarget;
    if (!input.gameModeValid) return Decision::MissingGameMode;
    if (!input.attackRouteAvailable) return Decision::MissingAttackRoute;

    if (!input.hitPointFinite ||
        !std::isfinite(input.distance) || input.distance < 0.0f ||
        !std::isfinite(input.maxDistance) || input.maxDistance <= 0.0f) {
        return Decision::InvalidHitPoint;
    }
    if (input.distance > input.maxDistance) return Decision::OutOfRange;
    if (!input.cooldownReady) return Decision::Cooldown;
    return Decision::Ready;
}

float effectiveScanRange(float aimAssistRange, bool autoHitEnabled, float autoHitRange,
                         float queryExpansionMargin) noexcept {
    const float normalizedAimRange = std::isfinite(aimAssistRange)
        ? std::clamp(aimAssistRange, 2.0f, 80.0f) : 5.5f;
    float baseRange = normalizedAimRange;
    if (autoHitEnabled) {
        const float normalizedAutoHitRange = std::isfinite(autoHitRange)
            ? std::clamp(autoHitRange, 2.0f, 6.0f) : 3.0f;
        baseRange = std::max(baseRange, normalizedAutoHitRange);
    }
    const float margin = std::isfinite(queryExpansionMargin)
        ? std::clamp(queryExpansionMargin, 0.0f, 4.0f) : 0.0f;
    return std::clamp(baseRange + margin, 2.0f, 80.0f);
}

void NoHitFeedbackGuard::reset() noexcept {
    m_target = nullptr;
    m_consecutiveNoFeedback = 0;
    m_blocked = false;
}

bool NoHitFeedbackGuard::isBlocked(const void* target) noexcept {
    if (!target) {
        reset();
        return false;
    }
    if (m_target != target) {
        reset();
        m_target = target;
    }
    return m_blocked;
}

bool NoHitFeedbackGuard::observe(const void* target, bool accepted, int hurtTimeBefore,
                                 int hurtTimeAfter) noexcept {
    if (!target) {
        reset();
        return false;
    }
    if (m_target != target) {
        reset();
        m_target = target;
    }
    if (m_blocked) {
        // Quarantine is retained as a diagnostic state only. AutoHit's CPS
        // limiter bounds dispatch, and delayed server state must not permanently
        // suppress valid attack attempts based on one immediate local snapshot.
        if (!accepted || hurtTimeAfter > hurtTimeBefore) {
            m_consecutiveNoFeedback = 0;
            m_blocked = false;
        }
        return false;
    }

    // A rejected wrapper call or a positive local hurt-time transition breaks
    // the "accepted but no immediate hit feedback" streak.
    if (!accepted || hurtTimeAfter > hurtTimeBefore) {
        m_consecutiveNoFeedback = 0;
        return false;
    }

    if (m_consecutiveNoFeedback < kAttemptsBeforeQuarantine)
        ++m_consecutiveNoFeedback;
    if (m_consecutiveNoFeedback >= kAttemptsBeforeQuarantine) {
        m_blocked = true;
        return true;
    }
    return false;
}

} // namespace bedaim::autohit
