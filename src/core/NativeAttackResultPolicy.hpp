#pragma once

namespace bedrocktools::core::attackresult {

// Reports whether the native route's own result was observed and passed its
// version-specific success criteria. This is not server-authoritative damage
// confirmation.
constexpr bool confirmedByNativeRoute(bool observerReached,
                                     bool internalNativeReturn,
                                     bool requiresOutputFlag,
                                     bool outputFlagUsed,
                                     bool outputFlagTrue) noexcept {
    return observerReached && internalNativeReturn &&
        (!requiresOutputFlag || (outputFlagUsed && outputFlagTrue));
}

} // namespace bedrocktools::core::attackresult
