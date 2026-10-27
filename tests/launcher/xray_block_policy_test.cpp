#include "modules/visual/xray_block_policy.hpp"

#include <cassert>

int main() {
    using bedaim::xray::OreKind;
    using bedaim::xray::colorForBlockName;
    using bedaim::xray::isTargetBlockName;
    using bedaim::xray::kindForBlockName;

    assert(isTargetBlockName("minecraft:diamond_ore"));
    assert(isTargetBlockName("minecraft:deepslate_diamond_ore"));
    assert(isTargetBlockName("minecraft:nether_quartz_ore"));
    assert(isTargetBlockName("minecraft:nether_gold_ore"));
    assert(isTargetBlockName("minecraft:ancient_debris"));
    assert(isTargetBlockName("my_addon:ruby_ore"));

    assert(!isTargetBlockName(""));
    assert(!isTargetBlockName("minecraft:stone"));
    assert(!isTargetBlockName("minecraft:raw_iron_block"));
    assert(!isTargetBlockName("minecraft:diamond_block"));
    assert(!isTargetBlockName("minecraft:ore"));

    assert(kindForBlockName("minecraft:deepslate_diamond_ore") == OreKind::Diamond);
    assert(kindForBlockName("minecraft:emerald_ore") == OreKind::Emerald);
    assert(kindForBlockName("minecraft:gold_ore") == OreKind::Gold);
    assert(kindForBlockName("minecraft:iron_ore") == OreKind::Iron);
    assert(kindForBlockName("minecraft:copper_ore") == OreKind::Copper);
    assert(kindForBlockName("minecraft:redstone_ore") == OreKind::Redstone);
    assert(kindForBlockName("minecraft:lapis_ore") == OreKind::Lapis);
    assert(kindForBlockName("minecraft:nether_quartz_ore") == OreKind::Quartz);
    assert(kindForBlockName("minecraft:coal_ore") == OreKind::Coal);
    assert(kindForBlockName("minecraft:ancient_debris") == OreKind::AncientDebris);
    assert(kindForBlockName("my_addon:ruby_ore") == OreKind::Other);
    assert(kindForBlockName("minecraft:stone") == OreKind::None);

    assert(colorForBlockName("minecraft:diamond_ore") == 0xFF42E8FFu);
    assert(colorForBlockName("minecraft:deepslate_emerald_ore") == 0xFF35F06Bu);
    assert(colorForBlockName("minecraft:gold_ore") == 0xFFFFD23Fu);
    assert(colorForBlockName("minecraft:iron_ore") == 0xFFE4E8EDu);
    assert(colorForBlockName("minecraft:copper_ore") == 0xFFFF8B45u);
    assert(colorForBlockName("minecraft:redstone_ore") == 0xFFFF3434u);
    assert(colorForBlockName("minecraft:lapis_ore") == 0xFF477BFFu);
    assert(colorForBlockName("minecraft:ancient_debris") == 0xFFB66A43u);
    assert(colorForBlockName("my_addon:ruby_ore") == 0xFFFFFFFFu);
    assert(colorForBlockName("minecraft:stone") == 0x00000000u);

    return 0;
}