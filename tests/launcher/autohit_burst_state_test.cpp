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

    // The caller preserves identity; the same pointer cannot re-arm the burst.
    assert(!bedaim::autohit::burstIdentityChanged(sword, targetA, sword, targetA));

    // A genuinely different target or selected weapon can arm a fresh burst.
    assert(bedaim::autohit::burstIdentityChanged(sword, targetA, sword, targetB));
    assert(bedaim::autohit::burstIdentityChanged(sword, targetA, "minecraft:mace", targetA));

    remaining = 0;
    bedaim::autohit::cancelBurstOnTargetLoss(remaining);
    assert(remaining == 0);
    return 0;
}
