#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace bedaim::autohit::weapon {

enum class Class { Sword, Axe, Mace, Trident, Unsupported };

inline Class classify(std::string_view identifier) {
    std::string normalized(identifier);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalized.find("sword") != std::string::npos) return Class::Sword;
    if (normalized.find("axe") != std::string::npos) return Class::Axe;
    if (normalized.find("mace") != std::string::npos) return Class::Mace;
    if (normalized.find("trident") != std::string::npos) return Class::Trident;
    return Class::Unsupported;
}

inline const char* name(Class weapon) noexcept {
    switch (weapon) {
        case Class::Sword: return "sword";
        case Class::Axe: return "axe";
        case Class::Mace: return "mace";
        case Class::Trident: return "trident";
        case Class::Unsupported: return "unsupported";
    }
    return "unsupported";
}

inline float normalizeCps(float cps) noexcept {
    return std::isfinite(cps) ? std::clamp(cps, 1.0f, 20.0f) : 1.0f;
}

} // namespace bedaim::autohit::weapon
