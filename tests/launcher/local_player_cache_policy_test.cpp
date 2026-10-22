#include "core/LocalPlayerCachePolicy.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
int failures = 0;
int checks = 0;

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}
}

int main() {
    using bedaim::core::clearCachedPointerIfUnchanged;

    using bedaim::core::shouldAcceptCameraActorCandidate;
    check(shouldAcceptCameraActorCandidate(true, true, true),
          "stable player owned by ClientInstance is accepted");
    check(!shouldAcceptCameraActorCandidate(false, true, true),
          "unstable camera actor is rejected");
    check(!shouldAcceptCameraActorCandidate(true, false, true),
          "non-player camera actor is rejected");
    check(!shouldAcceptCameraActorCandidate(true, true, false),
          "player not owned by ClientInstance is rejected");
    check(!shouldAcceptCameraActorCandidate(false, false, true),
          "unstable non-player candidate is rejected");
    check(!shouldAcceptCameraActorCandidate(true, false, false),
          "unowned non-player candidate is rejected");

    void* stale = reinterpret_cast<void*>(std::uintptr_t{0x1000});
    void* fresh = reinterpret_cast<void*>(std::uintptr_t{0x2000});
    std::atomic<void*> cache{stale};

    check(clearCachedPointerIfUnchanged(cache, stale),
          "matching stale cache value is invalidated");
    check(cache.load(std::memory_order_acquire) == nullptr,
          "successful invalidation leaves the cache empty");

    cache.store(fresh, std::memory_order_release);
    check(!clearCachedPointerIfUnchanged(cache, stale),
          "a concurrent replacement is not cleared");
    check(cache.load(std::memory_order_acquire) == fresh,
          "the fresh pointer survives a stale compare-and-clear");

    check(!clearCachedPointerIfUnchanged(cache, nullptr),
          "a null observation never clears the cache");
    check(cache.load(std::memory_order_acquire) == fresh,
          "a null observation preserves the current pointer");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
