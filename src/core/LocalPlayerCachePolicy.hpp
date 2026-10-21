#pragma once

#include <atomic>

namespace bedaim::core {

// Clear a cached pointer only if it still equals the value observed before a
// potentially slow validation. A concurrent replacement must never be erased.
inline bool clearCachedPointerIfUnchanged(
    std::atomic<void*>& cache, void* observed) noexcept {
    if (!observed) return false;
    return cache.compare_exchange_strong(
        observed, nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

} // namespace bedaim::core
