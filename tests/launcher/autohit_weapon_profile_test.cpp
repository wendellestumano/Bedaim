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

    using bedaim::autohit::weapon::resolve;
    auto profile = resolve(Class::Sword, true, 8.0f, false, 4.0f,
                           false, 2.0f, false, 3.0f);
    assert(profile.enabled && profile.cps == 8.0f);
    profile = resolve(Class::Axe, true, 8.0f, false, 4.0f,
                      false, 2.0f, false, 3.0f);
    assert(!profile.enabled && profile.cps == 4.0f);
    profile = resolve(Class::Mace, true, 8.0f, true, 4.0f,
                      true, 2.0f, false, 3.0f);
    assert(profile.enabled && profile.cps == 2.0f);
    profile = resolve(Class::Unsupported, true, 8.0f, true, 4.0f,
                      true, 2.0f, true, 3.0f);
    assert(!profile.enabled && profile.cps == 0.0f);
    return 0;
}
