#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>

namespace bedaim::visual {

struct BlockEventPosition {
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const BlockEventPosition&) const noexcept = default;
};

struct BlockEventCandidate {
    void* source = nullptr;
    BlockEventPosition position{};
    std::array<char, 128> blockName{};
    std::uint8_t blockNameLength = 0;
};

struct BlockEventEnqueueOutcome {
    bool inserted = false;
    bool merged = false;
    bool droppedOldest = false;
};

template <std::size_t Capacity, std::size_t DedupCapacity>
class BlockEventQueue {
    static_assert(Capacity > 0);
    static_assert(DedupCapacity >= Capacity * 2);
    static_assert((DedupCapacity & (DedupCapacity - 1)) == 0,
                  "DedupCapacity must be a power of two");

    struct DedupEntry {
        BlockEventPosition position{};
        std::size_t queueIndex = 0;
        std::uint8_t state = 0; // 0=empty, 1=occupied, 2=tombstone
    };

    static constexpr std::size_t kNotFound = DedupCapacity;

    static std::size_t hashPosition(const BlockEventPosition& position) noexcept {
        std::uint64_t h = static_cast<std::uint32_t>(position.x) + 0x9e3779b97f4a7c15ull;
        h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(position.y)) +
             0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(position.z)) +
             0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ull;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebull;
        h ^= h >> 31;
        return static_cast<std::size_t>(h) & (DedupCapacity - 1);
    }

    std::size_t findExisting(const BlockEventPosition& position) const noexcept {
        std::size_t slot = hashPosition(position);
        for (std::size_t probe = 0; probe < DedupCapacity; ++probe) {
            const auto& entry = m_dedup[slot];
            if (entry.state == 0) return kNotFound;
            if (entry.state == 1 && entry.position == position) return slot;
            slot = (slot + 1) & (DedupCapacity - 1);
        }
        return kNotFound;
    }

    std::size_t findInsertionSlot(const BlockEventPosition& position) const noexcept {
        std::size_t slot = hashPosition(position);
        std::size_t firstTombstone = kNotFound;
        for (std::size_t probe = 0; probe < DedupCapacity; ++probe) {
            const auto& entry = m_dedup[slot];
            if (entry.state == 0) return firstTombstone == kNotFound ? slot : firstTombstone;
            if (entry.state == 2 && firstTombstone == kNotFound) firstTombstone = slot;
            slot = (slot + 1) & (DedupCapacity - 1);
        }
        return firstTombstone;
    }

    void eraseDedup(const BlockEventPosition& position) noexcept {
        const auto slot = findExisting(position);
        if (slot == kNotFound) return;
        m_dedup[slot].state = 2;
    }

    void removeOldest() noexcept {
        eraseDedup(m_candidates[m_head].position);
        m_head = (m_head + 1) % Capacity;
        --m_count;
    }

    mutable std::mutex m_mutex;
    std::array<BlockEventCandidate, Capacity> m_candidates{};
    std::array<DedupEntry, DedupCapacity> m_dedup{};
    std::size_t m_head = 0;
    std::size_t m_count = 0;

public:
    BlockEventEnqueueOutcome enqueue(void* source, const BlockEventPosition& position,
                                     std::string_view resolvedBlockName = {}) noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        BlockEventEnqueueOutcome outcome{};

        // Match Apollon's bounded candidate semantics: one pending record per
        // block coordinate. If a later tessellation callback knows the name,
        // upgrade an earlier position-only event instead of consuming another slot.
        const auto existing = findExisting(position);
        if (existing != kNotFound) {
            auto& candidate = m_candidates[m_dedup[existing].queueIndex];
            candidate.source = source;
            if (!resolvedBlockName.empty()) {
                const auto length = std::min(resolvedBlockName.size(), candidate.blockName.size() - 1);
                std::memcpy(candidate.blockName.data(), resolvedBlockName.data(), length);
                candidate.blockName[length] = '\0';
                candidate.blockNameLength = static_cast<std::uint8_t>(length);
            }
            outcome.merged = true;
            return outcome;
        }

        if (m_count == Capacity) {
            removeOldest();
            outcome.droppedOldest = true;
        }

        const auto dedupSlot = findInsertionSlot(position);
        if (dedupSlot == kNotFound) return outcome;

        const std::size_t tail = (m_head + m_count) % Capacity;
        auto& candidate = m_candidates[tail];
        candidate = {};
        candidate.source = source;
        candidate.position = position;
        if (!resolvedBlockName.empty()) {
            const auto length = std::min(resolvedBlockName.size(), candidate.blockName.size() - 1);
            std::memcpy(candidate.blockName.data(), resolvedBlockName.data(), length);
            candidate.blockName[length] = '\0';
            candidate.blockNameLength = static_cast<std::uint8_t>(length);
        }

        auto& entry = m_dedup[dedupSlot];
        entry.position = position;
        entry.queueIndex = tail;
        entry.state = 1;
        ++m_count;
        outcome.inserted = true;
        return outcome;
    }

    std::vector<BlockEventCandidate> drain(std::size_t maximum) {
        std::vector<BlockEventCandidate> result;
        std::lock_guard<std::mutex> lock(m_mutex);
        result.reserve(std::min(maximum, m_count));
        const std::size_t count = std::min(maximum, m_count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& candidate = m_candidates[m_head];
            result.push_back(candidate);
            eraseDedup(candidate.position);
            m_head = (m_head + 1) % Capacity;
            --m_count;
        }
        return result;
    }

    void clear() noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_head = 0;
        m_count = 0;
        m_dedup.fill({});
    }

    std::size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_count;
    }
};

} // namespace bedaim::visual
