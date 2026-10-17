#include "modules/player/AutoHitWeaponProfile.hpp"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using bedaim::autohit::weapon::Class;
    using bedaim::autohit::weapon::classify;
    using bedaim::autohit::weapon::normalizeCps;

    assert(classify("minecraft:diamond_sword") == Class::Sword);
    assert(classify("minecraft:netherite_axe") == Class::Axe);
    assert(classify("minecraft:diamond_pickaxe") == Class::Unsupported);
    assert(classify("minecraft:mace") == Class::Mace);
    assert(classify("minecraft:trident") == Class::Trident);
    assert(classify("minecraft:bow") == Class::Unsupported);
    assert(classify("") == Class::Unsupported);

    assert(normalizeCps(8.0f) == 8.0f);
    assert(normalizeCps(0.0f) == 1.0f);
    assert(normalizeCps(40.0f) == 20.0f);
    assert(normalizeCps(std::numeric_limits<float>::quiet_NaN()) == 1.0f);
    return 0;
}
