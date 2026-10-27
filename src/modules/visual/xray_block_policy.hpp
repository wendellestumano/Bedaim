#pragma once

#include <cstdint>
#include <string_view>

namespace bedaim::xray {

enum class OreKind : std::uint8_t {
    None,
    Diamond,
    Emerald,
    Gold,
    Iron,
    Copper,
    Redstone,
    Lapis,
    Quartz,
    Coal,
    AncientDebris,
    Other
};

inline std::string_view stripNamespace(std::string_view name) noexcept {
    const std::size_t separator = name.find(':');
    if (separator != std::string_view::npos) name.remove_prefix(separator + 1);
    return name;
}

inline OreKind kindForBlockName(std::string_view rawName) noexcept {
    const std::string_view name = stripNamespace(rawName);
    if (name.empty()) return OreKind::None;
    if (name == "ancient_debris") return OreKind::AncientDebris;
    if (!name.ends_with("_ore")) return OreKind::None;
    if (name.find("diamond") != std::string_view::npos) return OreKind::Diamond;
    if (name.find("emerald") != std::string_view::npos) return OreKind::Emerald;
    if (name.find("gold") != std::string_view::npos) return OreKind::Gold;
    if (name.find("iron") != std::string_view::npos) return OreKind::Iron;
    if (name.find("copper") != std::string_view::npos) return OreKind::Copper;
    if (name.find("redstone") != std::string_view::npos) return OreKind::Redstone;
    if (name.find("lapis") != std::string_view::npos) return OreKind::Lapis;
    if (name.find("quartz") != std::string_view::npos) return OreKind::Quartz;
    if (name.find("coal") != std::string_view::npos) return OreKind::Coal;
    return OreKind::Other;
}

inline bool isTargetBlockName(std::string_view rawName) noexcept {
    return kindForBlockName(rawName) != OreKind::None;
}

// Opaque ARGB colors, selected for contrast in dark caves.
inline std::uint32_t colorForOreKind(OreKind kind) noexcept {
    switch (kind) {
        case OreKind::Diamond:       return 0xFF42E8FFu;
        case OreKind::Emerald:       return 0xFF35F06Bu;
        case OreKind::Gold:          return 0xFFFFD23Fu;
        case OreKind::Iron:          return 0xFFE4E8EDu;
        case OreKind::Copper:        return 0xFFFF8B45u;
        case OreKind::Redstone:      return 0xFFFF3434u;
        case OreKind::Lapis:         return 0xFF477BFFu;
        case OreKind::Quartz:        return 0xFFFFE8D2u;
        case OreKind::Coal:          return 0xFFB7B7C0u;
        case OreKind::AncientDebris: return 0xFFB66A43u;
        case OreKind::Other:         return 0xFFFFFFFFu;
        case OreKind::None:          return 0x00000000u;
    }
    return 0x00000000u;
}

inline std::uint32_t colorForBlockName(std::string_view rawName) noexcept {
    return colorForOreKind(kindForBlockName(rawName));
}

} // namespace bedaim::xray
