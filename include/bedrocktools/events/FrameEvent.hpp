#pragma once

#include <bedrocktools/events/Event.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace bedrocktools::events {

struct FrameEvent : Cancellable {
    static constexpr EventType type = EventType::Frame;

    // A shared deadline lets the game-update and EGL presentation paths pause
    // against the same window instead of sleeping independently.
    static void requestGlobalHitStop(std::chrono::milliseconds duration) noexcept {
        duration = std::clamp(duration, std::chrono::milliseconds(0),
                              std::chrono::milliseconds(750));
        if (duration.count() <= 0) return;

        const auto until = std::chrono::steady_clock::now() + duration;
        const auto requestedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            until.time_since_epoch()).count();
        auto currentNs = s_globalHoldUntilNs.load(std::memory_order_relaxed);
        while (requestedNs > currentNs &&
               !s_globalHoldUntilNs.compare_exchange_weak(
                   currentNs, requestedNs, std::memory_order_release,
                   std::memory_order_relaxed)) {}
    }

    static void waitForGlobalHitStop() noexcept {
        for (;;) {
            const auto deadlineNs = s_globalHoldUntilNs.load(std::memory_order_acquire);
            if (deadlineNs <= 0) return;

            const auto deadline = std::chrono::steady_clock::time_point(
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::nanoseconds(deadlineNs)));
            if (std::chrono::steady_clock::now() >= deadline) return;
            std::this_thread::sleep_until(deadline);
        }
    }

    // Retained for source compatibility with other FrameEvent listeners.
    void requestPostSwapHold(std::chrono::milliseconds duration) noexcept {
        duration = std::clamp(duration, std::chrono::milliseconds(0),
                              std::chrono::milliseconds(750));
        if (duration.count() <= 0) return;
        const auto requestedUntil = std::chrono::steady_clock::now() + duration;
        if (requestedUntil > m_postSwapHoldUntil) m_postSwapHoldUntil = requestedUntil;
    }

    std::chrono::steady_clock::time_point postSwapHoldUntil() const noexcept {
        return m_postSwapHoldUntil;
    }

private:
    inline static std::atomic<std::int64_t> s_globalHoldUntilNs{0};
    std::chrono::steady_clock::time_point m_postSwapHoldUntil{};
};

}
