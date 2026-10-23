#include "modules/player/AutoHitBurstState.hpp"

#include <cassert>
#include <string>

int main() {
    const int targetAObject = 1;
    const int targetBObject = 2;
    const void* targetA = &targetAObject;
    const void* targetB = &targetBObject;
    const std::string sword = "minecraft:diamond_sword";

    assert(!bedaim::autohit::burstIdentityChanged(sword, targetA, sword, targetA));

    int remaining = 2;
    bedaim::autohit::cancelBurstOnTargetLoss(remaining);
    assert(remaining == 0);
    // The same target identity must not re-arm the partially consumed burst.
    assert(!bedaim::autohit::burstIdentityChanged(sword, targetA, sword, targetA));

    // A different target or selected weapon may start a fresh burst.
    assert(bedaim::autohit::burstIdentityChanged(sword, targetA, sword, targetB));
    assert(bedaim::autohit::burstIdentityChanged(sword, targetA, "minecraft:mace", targetA));

    remaining = 0;
    bedaim::autohit::cancelBurstOnTargetLoss(remaining);
    assert(remaining == 0);
    return 0;
}
