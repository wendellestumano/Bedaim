#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <string>
#include <string_view>

namespace bedaim::autohit::weapon {

enum class Class { Sword, Axe, Mace, Trident, Spear, Bow, Crossbow, Unsupported };

inline Class classify(std::string_view identifier) {
    std::string normalized(identifier);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalized.find("sword") != std::string::npos) return Class::Sword;
    if (normalized.find("mace") != std::string::npos) return Class::Mace;
    if (normalized.find("trident") != std::string::npos) return Class::Trident;
    if (normalized.find("spear") != std::string::npos) return Class::Spear;
    if (normalized.find("crossbow") != std::string::npos) return Class::Crossbow;
    if (normalized.find("bow") != std::string::npos) return Class::Bow;
    if (normalized == "axe" || normalized.ends_with(":axe") ||
        normalized.find("_axe") != std::string::npos) return Class::Axe;
    return Class::Unsupported;
}

inline const char* name(Class weapon) noexcept {
    switch (weapon) {
        case Class::Sword: return "sword";
        case Class::Axe: return "axe";
        case Class::Mace: return "mace";
        case Class::Trident: return "trident";
        case Class::Spear: return "spear";
        case Class::Bow: return "bow";
        case Class::Crossbow: return "crossbow";
        case Class::Unsupported: return "unsupported";
    }
    return "unsupported";
}

// Vanilla IDs are listed explicitly, while the custom-ID text field lets users
// add weapons from add-ons/mods whose identifiers are not known at build time.
struct CatalogEntry {
    std::string_view identifier;
    std::string_view displayName;
};

inline constexpr std::array<CatalogEntry, 25> kWeaponCatalog{{
    {"minecraft:mace", "Mace"},
    {"minecraft:diamond_sword", "Diamond Sword"},
    {"minecraft:wooden_sword", "Wooden Sword"},
    {"minecraft:stone_sword", "Stone Sword"},
    {"minecraft:copper_sword", "Copper Sword"},
    {"minecraft:golden_sword", "Golden Sword"},
    {"minecraft:iron_sword", "Iron Sword"},
    {"minecraft:netherite_sword", "Netherite Sword"},
    {"minecraft:wooden_axe", "Wooden Axe"},
    {"minecraft:stone_axe", "Stone Axe"},
    {"minecraft:copper_axe", "Copper Axe"},
    {"minecraft:golden_axe", "Golden Axe"},
    {"minecraft:iron_axe", "Iron Axe"},
    {"minecraft:diamond_axe", "Diamond Axe"},
    {"minecraft:netherite_axe", "Netherite Axe"},
    {"minecraft:wooden_spear", "Wooden Spear"},
    {"minecraft:stone_spear", "Stone Spear"},
    {"minecraft:copper_spear", "Copper Spear"},
    {"minecraft:golden_spear", "Golden Spear"},
    {"minecraft:iron_spear", "Iron Spear"},
    {"minecraft:diamond_spear", "Diamond Spear"},
    {"minecraft:netherite_spear", "Netherite Spear"},
    {"minecraft:trident", "Trident"},
    {"minecraft:bow", "Bow"},
    {"minecraft:crossbow", "Crossbow"},
}};

inline std::string normalizeIdentifier(std::string_view identifier) {
    std::size_t begin = 0;
    std::size_t end = identifier.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(identifier[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(identifier[end - 1]))) --end;

    std::string normalized(identifier.substr(begin, end - begin));
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // InventoryAccess currently reads Item::getDescriptionId, which can return
    // localization keys (item.minecraft.diamond_sword) instead of registry IDs.
    // Canonicalize both forms so profile matching uses one stable key.
    if (normalized.starts_with("item.")) {
        normalized.erase(0, 5);
        const auto separator = normalized.find('.');
        if (separator == std::string::npos) {
            normalized = "minecraft:" + normalized;
        } else {
            const auto namespaceName = normalized.substr(0, separator);
            const auto itemName = normalized.substr(separator + 1);
            normalized = namespaceName + ":" + itemName;
        }
    } else if (!normalized.empty() && normalized.find(':') == std::string::npos) {
        normalized = "minecraft:" + normalized;
    }
    return normalized;
}

inline std::string displayNameForIdentifier(std::string_view identifier) {
    std::string normalized = normalizeIdentifier(identifier);
    const auto colon = normalized.find(':');
    std::string name = colon == std::string::npos ? normalized : normalized.substr(colon + 1);
    for (char& ch : name) {
        if (ch == '_' || ch == '-') ch = ' ';
    }
    bool capitalize = true;
    for (char& ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isspace(c)) {
            capitalize = true;
        } else if (capitalize) {
            ch = static_cast<char>(std::toupper(c));
            capitalize = false;
        }
    }
    return name.empty() ? std::string("Custom Weapon") : name;
}

inline float normalizeRange(float range) noexcept {
    return std::isfinite(range) ? std::clamp(range, 1.0f, 6.0f) : 3.2f;
}

inline float normalizeNormalCps(float cps) noexcept {
    return std::isfinite(cps) ? std::clamp(cps, 1.0f, 12.0f) : 5.0f;
}

inline float normalizeBurstCps(float cps) noexcept {
    return std::isfinite(cps) ? std::clamp(cps, 1.0f, 12.0f) : 12.0f;
}

inline int normalizeBurstHits(int hits) noexcept {
    return std::clamp(hits, 0, 20);
}

inline float normalizeCps(float cps) noexcept {
    // Preserve the original generic rate helper's invalid-value fallback.
    return std::isfinite(cps) ? std::clamp(cps, 1.0f, 20.0f) : 1.0f;
}

inline int attemptIntervalMs(float cps) noexcept {
    const float normalized = normalizeCps(cps);
    return static_cast<int>(std::lround(1000.0f / normalized));
}

struct Profile {
    bool enabled = false;
    float cps = 0.0f;
};

struct CustomProfile {
    std::string identifier;
    std::string displayName;
    bool enabled = false;
    float range = 3.2f;
    float cps = 5.0f;
    int initialBurstHits = 0;
    float initialBurstCps = 12.0f;
};

inline CustomProfile normalizeCustomProfile(CustomProfile profile) {
    profile.identifier = normalizeIdentifier(profile.identifier);
    if (profile.displayName.empty())
        profile.displayName = displayNameForIdentifier(profile.identifier);
    if (profile.displayName.size() > 96) profile.displayName.resize(96);
    profile.range = normalizeRange(profile.range);
    profile.cps = normalizeNormalCps(profile.cps);
    profile.initialBurstHits = normalizeBurstHits(profile.initialBurstHits);
    profile.initialBurstCps = normalizeBurstCps(profile.initialBurstCps);
    return profile;
}

inline bool matchesIdentifier(const CustomProfile& profile, std::string_view identifier) {
    const auto normalized = normalizeIdentifier(identifier);
    return !normalized.empty() && profile.identifier == normalized;
}

inline Profile resolve(Class weapon,
                       bool swordEnabled, float swordCps,
                       bool axeEnabled, float axeCps,
                       bool maceEnabled, float maceCps,
                       bool tridentEnabled, float tridentCps) noexcept {
    switch (weapon) {
        case Class::Sword: return {swordEnabled, normalizeNormalCps(swordCps)};
        case Class::Axe: return {axeEnabled, normalizeNormalCps(axeCps)};
        case Class::Mace: return {maceEnabled, normalizeNormalCps(maceCps)};
        case Class::Trident: return {tridentEnabled, normalizeNormalCps(tridentCps)};
        case Class::Spear:
        case Class::Bow:
        case Class::Crossbow:
        case Class::Unsupported: return {false, 0.0f};
    }
    return {false, 0.0f};
}

} // namespace bedaim::autohit::weapon
