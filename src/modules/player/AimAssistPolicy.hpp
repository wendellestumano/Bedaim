#pragma once

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

} // namespace bedaim::aimpolicy
