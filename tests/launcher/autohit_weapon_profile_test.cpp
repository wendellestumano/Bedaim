#include "modules/player/AutoHitWeaponProfile.hpp"

#include <cassert>
#include <cmath>
#include <limits>
#include <utility>

int main() {
    using bedaim::autohit::weapon::Class;
    using bedaim::autohit::weapon::classify;
    using bedaim::autohit::weapon::normalizeCps;

    assert(classify("minecraft:diamond_sword") == Class::Sword);
    assert(classify("minecraft:netherite_axe") == Class::Axe);
    assert(classify("minecraft:diamond_pickaxe") == Class::Unsupported);
    assert(classify("minecraft:mace") == Class::Mace);
    assert(classify("minecraft:trident") == Class::Trident);
    assert(classify("minecraft:bow") == Class::Bow);
    assert(classify("minecraft:crossbow") == Class::Crossbow);
    assert(classify("minecraft:diamond_spear") == Class::Spear);
    assert(classify("") == Class::Unsupported);

    assert(normalizeCps(8.0f) == 8.0f);
    assert(normalizeCps(0.0f) == 1.0f);
    assert(normalizeCps(40.0f) == 20.0f);
    assert(normalizeCps(std::numeric_limits<float>::quiet_NaN()) == 1.0f);

    using bedaim::autohit::weapon::attemptIntervalMs;
    assert(attemptIntervalMs(20.0f) == 50);
    assert(attemptIntervalMs(10.0f) == 100);
    assert(attemptIntervalMs(1.0f) == 1000);
    assert(attemptIntervalMs(0.0f) == 1000);
    assert(attemptIntervalMs(std::numeric_limits<float>::infinity()) == 1000);

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
    profile = resolve(Class::Bow, true, 8.0f, true, 4.0f,
                      true, 2.0f, true, 3.0f);
    assert(!profile.enabled && profile.cps == 0.0f);
    profile = resolve(Class::Unsupported, true, 8.0f, true, 4.0f,
                      true, 2.0f, true, 3.0f);
    assert(!profile.enabled && profile.cps == 0.0f);
    
    using bedaim::autohit::weapon::CustomProfile;
    using bedaim::autohit::weapon::normalizeIdentifier;
    using bedaim::autohit::weapon::normalizeRange;
    using bedaim::autohit::weapon::normalizeNormalCps;
    using bedaim::autohit::weapon::normalizeBurstCps;
    using bedaim::autohit::weapon::normalizeBurstHits;
    using bedaim::autohit::weapon::normalizeCustomProfile;
    using bedaim::autohit::weapon::matchesIdentifier;
    assert(normalizeIdentifier("  MACE  ") == "minecraft:mace");
    assert(normalizeIdentifier("item.minecraft.diamond_sword") == "minecraft:diamond_sword");
    assert(normalizeIdentifier("item.minecraft.mace") == "minecraft:mace");
    assert(normalizeIdentifier("item.fireworks") == "minecraft:firework_rocket");
    assert(normalizeIdentifier("minecraft:fireworks") == "minecraft:firework_rocket");
    assert(normalizeIdentifier("item.minecraft.fireworks") == "minecraft:firework_rocket");
    assert(normalizeIdentifier("minecraft:fireworks_rocket") == "minecraft:firework_rocket");
    assert(matchesNormalizedIdentifier("minecraft:firework_rocket", "item.fireworks"));
    assert(matchesNormalizedIdentifier("minecraft:fireworks", "minecraft:firework_rocket"));
    assert(normalizeIdentifier("item.myaddon.blazing_sword") == "myaddon:blazing_sword");
    assert(normalizeIdentifier("MyAddon:Blazing_Sword") == "myaddon:blazing_sword");
    assert(normalizeRange(4.5f) == 4.5f);
    assert(normalizeRange(15.0f) == 15.0f);
    assert(normalizeRange(100.0f) == 15.0f);
    assert(normalizeRange(std::numeric_limits<float>::quiet_NaN()) == 3.2f);
    assert(normalizeNormalCps(13.0f) == 12.0f);
    assert(normalizeBurstCps(13.0f) == 12.0f);
    assert(normalizeBurstCps(50.0f) == 12.0f);
    assert(normalizeBurstHits(-1) == 0);
    assert(normalizeBurstHits(21) == 20);
    CustomProfile custom{
        .identifier = "MINECRAFT:DIAMOND_SWORD",
        .displayName = "",
        .enabled = true,
        .range = 4.5f,
        .cps = 5.5f,
        .initialBurstHits = 3,
        .initialBurstCps = 12.0f
    };
    custom = normalizeCustomProfile(std::move(custom));
    assert(custom.identifier == "minecraft:diamond_sword");
    assert(custom.displayName == "Diamond Sword");
    assert(custom.range == 4.5f && custom.cps == 5.5f);
    assert(custom.initialBurstHits == 3 && custom.initialBurstCps == 12.0f);
    assert(matchesIdentifier(custom, "minecraft:diamond_sword"));
    assert(!matchesIdentifier(custom, "minecraft:netherite_sword"));
    CustomProfile fireworks{.identifier = "Minecraft:firework_rocket", .enabled = true};
    fireworks = normalizeCustomProfile(std::move(fireworks));
    assert(matchesIdentifier(fireworks, "item.fireworks"));
    assert(matchesIdentifier(fireworks, "minecraft:firework_rocket"));
    CustomProfile allSwords{.identifier = "minecraft:sword", .enabled = true};
    allSwords = normalizeCustomProfile(std::move(allSwords));
    assert(matchesIdentifier(allSwords, "minecraft:diamond_sword"));
    assert(matchesIdentifier(allSwords, "minecraft:netherite_sword"));
    assert(matchesIdentifier(allSwords, "item.minecraft.wooden_sword"));
    assert(!matchesIdentifier(allSwords, "minecraft:diamond_axe"));
    CustomProfile allAxes{.identifier = "minecraft:axe", .enabled = true};
    allAxes = normalizeCustomProfile(std::move(allAxes));
    assert(matchesIdentifier(allAxes, "minecraft:netherite_axe"));
    assert(!matchesIdentifier(allAxes, "minecraft:diamond_sword"));
    CustomProfile allBows{.identifier = "minecraft:bow", .enabled = true};
    allBows = normalizeCustomProfile(std::move(allBows));
    assert(matchesIdentifier(allBows, "myaddon:longbow"));
    assert(!matchesIdentifier(allBows, "minecraft:diamond_sword"));
    assert(!matchesIdentifier(custom, ""));

    return 0;
}
