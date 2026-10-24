#include "modules/visual/xray_block_policy.hpp"

#include <cassert>

int main() {
    using bedaim::xray::isTargetBlockName;

    assert(isTargetBlockName("minecraft:diamond_ore"));
    assert(isTargetBlockName("minecraft:deepslate_diamond_ore"));
    assert(isTargetBlockName("minecraft:nether_quartz_ore"));
    assert(isTargetBlockName("minecraft:ancient_debris"));
    assert(isTargetBlockName("my_addon:ruby_ore"));

    assert(!isTargetBlockName(""));
    assert(!isTargetBlockName("minecraft:stone"));
    assert(!isTargetBlockName("minecraft:raw_iron_block"));
    assert(!isTargetBlockName("minecraft:diamond_block"));
    assert(!isTargetBlockName("minecraft:ore"));

    return 0;
}
