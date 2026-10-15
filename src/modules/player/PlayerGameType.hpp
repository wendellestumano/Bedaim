#pragma once

#include <cstdint>

namespace bedaim::playergametype {

// Canonical IDs used by Bedaim: Survival=0, Creative=1, Adventure=2, Spectator=3.
// Native IDs differ between the legacy Bedrock 1.26.51/1.26.52 layout and
// the shifted GameType enum used by 1.26.60-preview.21 and later.
inline bool normalizeNativeValue(std::int32_t nativeValue, bool shiftedLayout,
                                std::int32_t& canonical) noexcept {
    if (shiftedLayout) {
        switch (nativeValue) {
            case 0: canonical = 0; return true;
            case 1: canonical = 1; return true;
            case 2: canonical = 2; return true;
            case 6: canonical = 3; return true;
            default: return false;
        }
    }
    switch (nativeValue) {
        case 1: canonical = 0; return true;
        case 2: canonical = 1; return true;
        case 3: canonical = 2; return true;
        case 5: canonical = 3; return true;
        default: return false;
    }
}

} // namespace bedaim::playergametype
