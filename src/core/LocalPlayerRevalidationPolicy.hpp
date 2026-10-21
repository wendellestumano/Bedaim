#pragma once
#include <cstdint>

namespace bedrocktools::core::gamehooks {

// Recheck a non-null cached local-player pointer every 16 valid actor ticks.
// Runtime ClientInstance and local-player getter probes retain their own 250 ms throttles.
constexpr bool shouldRevalidateCachedLocalPlayer(std::uint32_t tickCount) noexcept {
    return (tickCount & 0x0Fu) == 0u;
}

} // namespace bedrocktools::core::gamehooks
