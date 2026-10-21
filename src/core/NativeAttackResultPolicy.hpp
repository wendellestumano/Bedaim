#pragma once

namespace bedrocktools::core::attackresult {

// Reports whether the validated native route itself accepted an attack attempt.
// This is not server-authoritative damage confirmation.
constexpr bool confirmedByNativeRoute(bool observerReached,
                                     bool internalNativeReturn,
                                     bool requiresOutputFlag,
                                     bool outputFlagUsed,
                                     bool outputFlagTrue,
                                     bool validatedPublicReturn = false) noexcept {
    if (requiresOutputFlag) {
        return observerReached && internalNativeReturn && outputFlagUsed && outputFlagTrue;
    }
    // If the internal observer was reached, its return is the best evidence.
    // Otherwise, accept the return only because the caller has independently
    // validated the exact public three-argument GameMode vtable adapter and ABI.
    return observerReached ? internalNativeReturn : validatedPublicReturn;
}

} // namespace bedrocktools::core::attackresult
