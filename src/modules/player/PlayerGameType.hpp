#pragma once

#include <cstdint>

namespace bedaim::playergametype {

// Canonical IDs for Aim Assist and Player Radar: Survival=0, Creative=1,
// Adventure=2, Spectator=3. Player::getPlayerGameType returns the native C++
// GameType enum; it does not return the values used by the network packet.
inline bool normalizeNativeValue(std::int32_t nativeValue,
                                std::int32_t& canonical) noexcept {
    switch (nativeValue) {
        case 0: canonical = 0; return true; // GameType::Survival
        case 1: canonical = 1; return true; // GameType::Creative
        case 2: canonical = 2; return true; // GameType::Adventure
        case 6: canonical = 3; return true; // GameType::Spectator
        default: return false; // Undefined=-1, Default=5, other unknown values
    }
}

} // namespace bedaim::playergametype
