#include "core/LocalPlayerRevalidationPolicy.hpp"
#include <cstdint>
int main() {
    using bedrocktools::core::gamehooks::shouldRevalidateCachedLocalPlayer;
    for (std::uint32_t i = 0; i < 512; ++i) {
        if (shouldRevalidateCachedLocalPlayer(i) != ((i & 15u) == 0u)) return 1;
    }
    return 0;
}
