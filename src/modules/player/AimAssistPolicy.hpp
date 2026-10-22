#pragma once

#include <string_view>

namespace bedaim::aimpolicy {

// Keep the ordinal values stable: AimAssist caches this category as an integer.
enum class HeldWeapon {
    None, Sword, Axe, Mace, Bow, Crossbow, Trident, Spear, OtherMelee, Tool
};

// In Ranged prediction mode, lead projectile/throwable weapons only.
// Bedrock 1.26.50.4 sample spear definitions use piercing/kinetic weapon
// components rather than a projectile component, so Spears remain melee here.
constexpr bool isRangedPredictionWeapon(HeldWeapon weapon) noexcept {
    return weapon == HeldWeapon::Bow ||
           weapon == HeldWeapon::Crossbow ||
           weapon == HeldWeapon::Trident;
}

// Treat ASCII letters/digits as identifier characters. Underscores, hyphens,
// and path separators delimit item-name tokens.
constexpr bool isIdentifierCharacter(char ch) noexcept {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9');
}

// Match a complete token so "rainbow_staff" cannot be classified as a Bow
// and "swordsmith:healing_wand" cannot be classified from its namespace.
constexpr bool containsToken(std::string_view value, std::string_view token) noexcept {
    std::size_t pos = 0;
    while ((pos = value.find(token, pos)) != std::string_view::npos) {
        const std::size_t end = pos + token.size();
        const bool leftBoundary = pos == 0 || !isIdentifierCharacter(value[pos - 1]);
        const bool rightBoundary = end == value.size() || !isIdentifierCharacter(value[end]);
        if (leftBoundary && rightBoundary) return true;
        ++pos;
    }
    return false;
}

constexpr std::string_view itemPath(std::string_view identifier) noexcept {
    const std::size_t namespaceSeparator = identifier.find(':');
    if (namespaceSeparator != std::string_view::npos)
        identifier.remove_prefix(namespaceSeparator + 1);
    return identifier;
}

constexpr HeldWeapon classifyHeldWeaponIdentifier(std::string_view identifier) noexcept {
    if (identifier.empty()) return HeldWeapon::None;
    const std::string_view path = itemPath(identifier);
    if (path.empty()) return HeldWeapon::None;

    // Specific classes first; token matching prevents "bow" inside "crossbow".
    if (containsToken(path, "crossbow")) return HeldWeapon::Crossbow;
    if (containsToken(path, "trident")) return HeldWeapon::Trident;
    if (containsToken(path, "mace")) return HeldWeapon::Mace;
    if (containsToken(path, "spear")) return HeldWeapon::Spear;
    if (containsToken(path, "sword")) return HeldWeapon::Sword;
    if (containsToken(path, "bow")) return HeldWeapon::Bow;

    if (containsToken(path, "dagger") || containsToken(path, "katana") ||
        containsToken(path, "rapier") || containsToken(path, "scythe") ||
        containsToken(path, "hammer") || containsToken(path, "halberd") ||
        containsToken(path, "glaive") || containsToken(path, "lance")) {
        return HeldWeapon::OtherMelee;
    }

    if (containsToken(path, "pickaxe") || containsToken(path, "shovel") ||
        containsToken(path, "hoe")) return HeldWeapon::Tool;
    if (containsToken(path, "axe")) return HeldWeapon::Axe;
    return HeldWeapon::None;
}

} // namespace bedaim::aimpolicy
