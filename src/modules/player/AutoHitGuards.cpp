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

float effectiveScanRange(float aimAssistRange, bool autoHitEnabled, float autoHitRange) noexcept {
    const float normalizedAimRange = std::isfinite(aimAssistRange)
        ? std::clamp(aimAssistRange, 2.0f, 80.0f) : 5.5f;
    if (!autoHitEnabled) return normalizedAimRange;
    const float normalizedAutoHitRange = std::isfinite(autoHitRange)
        ? std::clamp(autoHitRange, 2.0f, 6.0f) : 3.0f;
    return std::max(normalizedAimRange, normalizedAutoHitRange);
}

} // namespace bedaim::autohit
