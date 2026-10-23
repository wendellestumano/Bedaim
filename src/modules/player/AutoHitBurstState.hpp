#pragma once

#include <string_view>

namespace bedaim::autohit {

inline bool burstIdentityChanged(std::string_view previousWeaponIdentifier,
                                 const void* previousTarget,
                                 std::string_view currentWeaponIdentifier,
                                 const void* currentTarget) noexcept {
    return previousWeaponIdentifier != currentWeaponIdentifier ||
           previousTarget != currentTarget;
}

// Cancel the remaining initial-burst requests without clearing the last identity.
// Keeping the identity prevents the same pointer from re-arming a burst after a
// temporary target-resolution gap.
inline void cancelBurstOnTargetLoss(int& remainingRequests) noexcept {
    remainingRequests = 0;
}

} // namespace bedaim::autohit
