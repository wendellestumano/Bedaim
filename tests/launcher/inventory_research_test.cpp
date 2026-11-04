#include "../../src/modules/misc/InventoryResearch.hpp"

#include <cassert>
#include <iostream>

using namespace bedaim::inventory;

static InventorySnapshot snapshot(
    SnapshotSource source,
    SnapshotPhase phase,
    std::initializer_list<SlotSnapshot> slots) {
    InventorySnapshot value;
    value.worldToken = "test-world";
    value.source = source;
    value.phase = phase;
    value.complete = true;
    value.slots = slots;
    return value;
}

int main() {
    const auto baseline = snapshot(SnapshotSource::LocalClient, SnapshotPhase::Baseline, {
        {0, SlotKnowledge::Item, "minecraft:diamond", 32},
        {1, SlotKnowledge::Empty, "", 0}
    });

    const auto unchanged = snapshot(SnapshotSource::LocalClient, SnapshotPhase::AfterOperation, {
        {0, SlotKnowledge::Item, "minecraft:diamond", 32},
        {1, SlotKnowledge::Empty, "", 0}
    });
    assert(classifyChange(baseline, unchanged) == ChangeClass::NoChange);

    const auto clientIncrease = snapshot(SnapshotSource::LocalClient, SnapshotPhase::AfterOperation, {
        {0, SlotKnowledge::Item, "minecraft:diamond", 32},
        {1, SlotKnowledge::Item, "minecraft:diamond", 32}
    });
    assert(classifyChange(baseline, clientIncrease) == ChangeClass::LocalOnly);

    const auto clientDecrease = snapshot(SnapshotSource::LocalClient, SnapshotPhase::AfterOperation, {
        {0, SlotKnowledge::Item, "minecraft:diamond", 16},
        {1, SlotKnowledge::Empty, "", 0}
    });
    assert(classifyChange(baseline, clientDecrease) == ChangeClass::LocalCountDecrease);

    const auto mixedChange = snapshot(SnapshotSource::LocalClient, SnapshotPhase::AfterOperation, {
        {0, SlotKnowledge::Item, "minecraft:emerald", 16},
        {1, SlotKnowledge::Empty, "", 0}
    });
    assert(classifyChange(baseline, mixedChange) == ChangeClass::LocalInventoryChanged);

    const auto corrected = snapshot(SnapshotSource::ServerAuthoritative, SnapshotPhase::AfterOperation, {
        {0, SlotKnowledge::Item, "minecraft:diamond", 32},
        {1, SlotKnowledge::Empty, "", 0}
    });
    assert(classifyChange(baseline, clientIncrease, &corrected) == ChangeClass::ServerCorrected);
    assert(classifyChange(baseline, clientDecrease, &corrected) == ChangeClass::ServerCorrected);

    const auto persistedIncrease = snapshot(SnapshotSource::ServerAuthoritative, SnapshotPhase::AfterReconnect, {
        {0, SlotKnowledge::Item, "minecraft:diamond", 32},
        {1, SlotKnowledge::Item, "minecraft:diamond", 32}
    });
    assert(classifyChange(baseline, clientIncrease, &persistedIncrease) == ChangeClass::PersistedCountIncrease);

    auto incomplete = clientIncrease;
    incomplete.slots[1].knowledge = SlotKnowledge::Unknown;
    assert(classifyChange(baseline, incomplete) == ChangeClass::Inconclusive);

    auto wrongWorld = corrected;
    wrongWorld.worldToken = "different-world";
    assert(classifyChange(baseline, clientIncrease, &wrongWorld) == ChangeClass::Inconclusive);

    auto invalidEmpty = unchanged;
    invalidEmpty.slots[1].count = 1;
    assert(classifyChange(baseline, invalidEmpty) == ChangeClass::Inconclusive);

    auto duplicateSlot = clientIncrease;
    duplicateSlot.slots[1].slotIndex = 0;
    assert(classifyChange(baseline, duplicateSlot) == ChangeClass::Inconclusive);

    auto wrongLayout = clientIncrease;
    wrongLayout.slots[1].slotIndex = 2;
    assert(classifyChange(baseline, wrongLayout) == ChangeClass::Inconclusive);

    auto invalidPhase = persistedIncrease;
    invalidPhase.phase = SnapshotPhase::Baseline;
    assert(classifyChange(baseline, clientIncrease, &invalidPhase) == ChangeClass::Inconclusive);

    std::cout << "Inventory research classification tests passed\n";
    return 0;
}
