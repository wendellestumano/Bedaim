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

bool hitResultMatchesTarget(const void* target, const void* hitResultTarget) noexcept {
    return target != nullptr && hitResultTarget == target;
}

void NoHitFeedbackGuard::reset() noexcept {
    m_target = nullptr;
    m_consecutiveNoFeedback = 0;
    m_warningEmitted = false;
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

    // A rejected call or positive local feedback resets the warning streak.
    if (!accepted || hurtTimeAfter > hurtTimeBefore) {
        m_consecutiveNoFeedback = 0;
        m_warningEmitted = false;
        return false;
    }

    if (m_consecutiveNoFeedback < kAttemptsBeforeWarning)
        ++m_consecutiveNoFeedback;
    if (m_consecutiveNoFeedback >= kAttemptsBeforeWarning && !m_warningEmitted) {
        m_warningEmitted = true;
        return true;
    }
    return false;
}

} // namespace bedaim::autohit
