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

// Kept free of native game pointers so the ordering rules can be host-tested.
constexpr bool contains(std::string_view value, std::string_view part) noexcept {
    return value.find(part) != std::string_view::npos;
}

constexpr HeldWeapon classifyHeldWeaponIdentifier(std::string_view identifier) noexcept {
    if (identifier.empty()) return HeldWeapon::None;

    // Specific classes first; notably Crossbow must precede Bow.
    if (contains(identifier, "crossbow")) return HeldWeapon::Crossbow;
    if (contains(identifier, "trident")) return HeldWeapon::Trident;
    if (contains(identifier, "mace")) return HeldWeapon::Mace;
    if (contains(identifier, "spear")) return HeldWeapon::Spear;
    if (contains(identifier, "sword")) return HeldWeapon::Sword;
    if (contains(identifier, "bow")) return HeldWeapon::Bow;

    if (contains(identifier, "dagger") || contains(identifier, "katana") ||
        contains(identifier, "rapier") || contains(identifier, "scythe") ||
        contains(identifier, "hammer") || contains(identifier, "halberd") ||
        contains(identifier, "glaive") || contains(identifier, "lance")) {
        return HeldWeapon::OtherMelee;
    }

    // Tool identifiers contain "axe", so they must precede generic axe matching.
    if (contains(identifier, "pickaxe") || contains(identifier, "shovel") ||
        contains(identifier, "hoe")) return HeldWeapon::Tool;
    if (contains(identifier, "axe")) return HeldWeapon::Axe;
    return HeldWeapon::None;
}

} // namespace bedaim::aimpolicy
