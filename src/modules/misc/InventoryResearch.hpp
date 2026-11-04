#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bedaim::inventory {

enum class SlotKnowledge : std::uint8_t {
    Unknown,
    Empty,
    Item
};

enum class SnapshotSource : std::uint8_t {
    LocalClient,
    ServerAuthoritative
};

enum class SnapshotPhase : std::uint8_t {
    Baseline,
    AfterOperation,
    AfterReconnect
};

enum class ChangeClass : std::uint8_t {
    NoChange,
    LocalOnly,
    ServerCorrected,
    PersistedCountIncrease,
    Inconclusive
};

struct SlotSnapshot {
    int slotIndex = -1;
    SlotKnowledge knowledge = SlotKnowledge::Unknown;
    std::string itemIdentifier;
    std::uint32_t count = 0;
};

struct InventorySnapshot {
    std::string worldToken;
    SnapshotSource source = SnapshotSource::LocalClient;
    SnapshotPhase phase = SnapshotPhase::Baseline;
    bool complete = false;
    std::vector<SlotSnapshot> slots;
};

using ItemTotals = std::map<std::string, std::uint64_t>;

inline bool aggregateTotals(const InventorySnapshot& snapshot, ItemTotals& out) {
    out.clear();
    if (!snapshot.complete || snapshot.worldToken.empty()) return false;

    for (const auto& slot : snapshot.slots) {
        if (slot.knowledge == SlotKnowledge::Unknown || slot.slotIndex < 0) return false;
        if (slot.knowledge == SlotKnowledge::Empty) {
            if (!slot.itemIdentifier.empty() || slot.count != 0) return false;
            continue;
        }
        if (slot.itemIdentifier.empty() || slot.count == 0) return false;
        out[slot.itemIdentifier] += slot.count;
    }
    return true;
}

inline bool hasIncrease(const ItemTotals& baseline, const ItemTotals& candidate) {
    for (const auto& [identifier, count] : candidate) {
        const auto it = baseline.find(identifier);
        const std::uint64_t previous = it == baseline.end() ? 0 : it->second;
        if (count > previous) return true;
    }
    return false;
}

inline bool totalsEqual(const ItemTotals& left, const ItemTotals& right) {
    return left == right;
}

// Classifies evidence only. PersistedCountIncrease is deliberately not named
// "dupe": item lineage/NBT and an independently verified second copy are not
// captured by this privacy-preserving count-only model.
inline ChangeClass classifyChange(
    const InventorySnapshot& baseline,
    const InventorySnapshot& localAfter,
    const InventorySnapshot* serverVerification = nullptr) {

    ItemTotals baselineTotals;
    ItemTotals localTotals;
    if (baseline.source != SnapshotSource::LocalClient ||
        baseline.phase != SnapshotPhase::Baseline ||
        localAfter.source != SnapshotSource::LocalClient ||
        localAfter.phase != SnapshotPhase::AfterOperation ||
        baseline.worldToken != localAfter.worldToken ||
        !aggregateTotals(baseline, baselineTotals) ||
        !aggregateTotals(localAfter, localTotals)) {
        return ChangeClass::Inconclusive;
    }

    if (!hasIncrease(baselineTotals, localTotals)) {
        return totalsEqual(baselineTotals, localTotals)
            ? ChangeClass::NoChange
            : ChangeClass::Inconclusive;
    }

    if (!serverVerification) return ChangeClass::LocalOnly;

    ItemTotals serverTotals;
    if (serverVerification->source != SnapshotSource::ServerAuthoritative ||
        serverVerification->worldToken != baseline.worldToken ||
        !aggregateTotals(*serverVerification, serverTotals)) {
        return ChangeClass::Inconclusive;
    }

    if (totalsEqual(baselineTotals, serverTotals)) {
        return ChangeClass::ServerCorrected;
    }

    if (serverVerification->phase == SnapshotPhase::AfterReconnect &&
        hasIncrease(baselineTotals, serverTotals)) {
        return ChangeClass::PersistedCountIncrease;
    }

    return ChangeClass::Inconclusive;
}

} // namespace bedaim::inventory
