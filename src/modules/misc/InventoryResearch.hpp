#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bedaim::inventory {

enum class SlotKnowledge : std::uint8_t { Unknown, Empty, Item };
enum class SnapshotSource : std::uint8_t { LocalClient, ServerAuthoritative };
enum class SnapshotPhase : std::uint8_t { Baseline, AfterOperation, AfterReconnect };
enum class ChangeClass : std::uint8_t {
    NoChange,
    LocalOnly,
    LocalCountDecrease,
    LocalInventoryChanged,
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
    if (!snapshot.complete || snapshot.worldToken.empty() || snapshot.slots.empty()) return false;

    std::set<int> seenSlots;
    for (const auto& slot : snapshot.slots) {
        if (slot.slotIndex < 0 || !seenSlots.insert(slot.slotIndex).second) return false;
        if (slot.knowledge == SlotKnowledge::Unknown) return false;
        if (slot.knowledge == SlotKnowledge::Empty) {
            if (!slot.itemIdentifier.empty() || slot.count != 0) return false;
            continue;
        }
        if (slot.knowledge != SlotKnowledge::Item ||
            slot.itemIdentifier.empty() || slot.count == 0) return false;

        auto& total = out[slot.itemIdentifier];
        if (slot.count > std::numeric_limits<std::uint64_t>::max() - total) return false;
        total += slot.count;
    }
    return true;
}

inline bool sameSlotLayout(const InventorySnapshot& left, const InventorySnapshot& right) {
    if (left.slots.size() != right.slots.size()) return false;
    std::vector<int> leftSlots;
    std::vector<int> rightSlots;
    leftSlots.reserve(left.slots.size());
    rightSlots.reserve(right.slots.size());
    for (const auto& slot : left.slots) leftSlots.push_back(slot.slotIndex);
    for (const auto& slot : right.slots) rightSlots.push_back(slot.slotIndex);
    std::sort(leftSlots.begin(), leftSlots.end());
    std::sort(rightSlots.begin(), rightSlots.end());
    return leftSlots == rightSlots;
}

inline bool hasIncrease(const ItemTotals& baseline, const ItemTotals& candidate) {
    for (const auto& [identifier, count] : candidate) {
        const auto it = baseline.find(identifier);
        const std::uint64_t previous = it == baseline.end() ? 0 : it->second;
        if (count > previous) return true;
    }
    return false;
}

inline bool hasDecrease(const ItemTotals& baseline, const ItemTotals& candidate) {
    for (const auto& [identifier, count] : baseline) {
        const auto it = candidate.find(identifier);
        const std::uint64_t current = it == candidate.end() ? 0 : it->second;
        if (current < count) return true;
    }
    return false;
}

inline bool totalsEqual(const ItemTotals& left, const ItemTotals& right) {
    return left == right;
}

// Classifies evidence only. Count-only snapshots cannot prove item lineage,
// independent copies, NBT equality, or a confirmed duplication.
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
        !sameSlotLayout(baseline, localAfter) ||
        !aggregateTotals(baseline, baselineTotals) ||
        !aggregateTotals(localAfter, localTotals)) {
        return ChangeClass::Inconclusive;
    }

    const bool increased = hasIncrease(baselineTotals, localTotals);
    const bool decreased = hasDecrease(baselineTotals, localTotals);
    if (!increased && !decreased) return ChangeClass::NoChange;

    if (!serverVerification) {
        if (increased && decreased) return ChangeClass::LocalInventoryChanged;
        if (increased) return ChangeClass::LocalOnly;
        if (decreased) return ChangeClass::LocalCountDecrease;
        return ChangeClass::Inconclusive;
    }

    ItemTotals serverTotals;
    if (serverVerification->source != SnapshotSource::ServerAuthoritative ||
        serverVerification->worldToken != baseline.worldToken ||
        (serverVerification->phase != SnapshotPhase::AfterOperation &&
         serverVerification->phase != SnapshotPhase::AfterReconnect) ||
        !sameSlotLayout(baseline, *serverVerification) ||
        !aggregateTotals(*serverVerification, serverTotals)) {
        return ChangeClass::Inconclusive;
    }

    if (totalsEqual(baselineTotals, serverTotals)) return ChangeClass::ServerCorrected;

    if (serverVerification->phase == SnapshotPhase::AfterReconnect &&
        hasIncrease(baselineTotals, serverTotals) &&
        !hasDecrease(baselineTotals, serverTotals)) {
        return ChangeClass::PersistedCountIncrease;
    }

    return ChangeClass::Inconclusive;
}

} // namespace bedaim::inventory
