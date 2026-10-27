#pragma once

#include <cstdint>
#include <string_view>

namespace bedaim::xray {

inline std::string_view stripNamespace(std::string_view name) noexcept {
    const std::size_t separator = name.find(':');
    if (separator != std::string_view::npos) name.remove_prefix(separator + 1);
    return name;
}

inline bool isTargetBlockName(std::string_view rawName) noexcept {
    const std::string_view name = stripNamespace(rawName);
    if (name.empty()) return false;
    // Ancient debris and nether gold ore do not use the usual *_ore suffix
    // consistently across versions.
    if (name == "ancient_debris" || name == "nether_gold_ore") return true;
    return name.ends_with("_ore");
}

// Opaque ARGB outline colors, chosen to remain distinguishable in dark caves.
inline std::uint32_t colorForBlockName(std::string_view rawName) noexcept {
    const std::string_view name = stripNamespace(rawName);
    if (name == "ancient_debris") return 0xFFB66A43u;
    if (name.find("diamond") != std::string_view::npos) return 0xFF42E8FFu;
    if (name.find("emerald") != std::string_view::npos) return 0xFF35F06Bu;
    if (name.find("gold") != std::string_view::npos) return 0xFFFFD23Fu;
    if (name.find("iron") != std::string_view::npos) return 0xFFE4E8EDu;
    if (name.find("copper") != std::string_view::npos) return 0xFFFF8B45u;
    if (name.find("redstone") != std::string_view::npos) return 0xFFFF3434u;
    if (name.find("lapis") != std::string_view::npos) return 0xFF477BFFu;
    if (name.find("quartz") != std::string_view::npos) return 0xFFFFE8D2u;
    if (name.find("coal") != std::string_view::npos) return 0xFFB7B7C0u;
    if (name.find("copper") != std::string_view::npos) return 0xFFFF8B45u;
    return 0xFFFFFFFFu;
}

} // namespace bedaim::xray
