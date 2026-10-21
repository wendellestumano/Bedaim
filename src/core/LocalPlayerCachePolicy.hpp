#pragma once

#include <atomic>

namespace bedaim::core {

// A camera actor is a LocalPlayer candidate only when its getter result is
// stable, it is classified as a Player, and the active ClientInstance owns it.
inline constexpr bool shouldAcceptCameraActorCandidate(
    bool stableReturn, bool actorIsPlayer, bool clientInstanceOwnsActor) noexcept {
    return stableReturn && actorIsPlayer && clientInstanceOwnsActor;
}

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
