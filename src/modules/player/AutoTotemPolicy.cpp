#include "AutoTotemPolicy.hpp"

#include <algorithm>
#include <cmath>

namespace bedaim::autototem {

Decision decide(const Inputs& input) noexcept {
    if (!input.enabled || !input.pending || input.offhandHasTotem) {
        return Decision::Idle;
    }

    // Never race a user-driven container transaction.
    if (input.containerOpen) return Decision::WaitForContainer;

    if (input.combatOnly && !input.inCombat) {
        return Decision::WaitForCombat;
    }

    if (!input.delayElapsed) return Decision::WaitForDelay;

    // Keep a shield or other held item intact. Refill only an empty offhand.
    if (!input.offhandEmpty) return Decision::WaitForOffhandClear;
    if (!input.candidateAvailable) return Decision::WaitForItem;

    return Decision::Equip;
}

float normalizeDelayMs(float value) noexcept {
    if (!std::isfinite(value)) return 100.0f;
    return std::clamp(value, 0.0f, 1000.0f);
}

float normalizeScanRetryMs(float value) noexcept {
    if (!std::isfinite(value)) return 400.0f;
    // A full inventory scan may resolve many native ItemStack identifiers.
    // Keep the retry interval above a quarter second to prevent repeated scans
    // every few ticks when the player has no spare totem.
    return std::clamp(value, 400.0f, 2000.0f);
}

float normalizeOffhandPollMs(float value) noexcept {
    if (!std::isfinite(value)) return 150.0f;
    // Offhand identifiers are native string lookups; polling at render cadence
    // can stall low-end Android devices. 150 ms keeps detection responsive while
    // avoiding a lookup on almost every frame at 10 FPS.
    return std::clamp(value, 150.0f, 500.0f);
}

float normalizeUiStepDelayMs(float value) noexcept {
    if (!std::isfinite(value)) return 80.0f;
    return std::clamp(value, 0.0f, 500.0f);
}

} // namespace bedaim::autototem
