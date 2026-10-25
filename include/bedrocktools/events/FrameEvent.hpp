#pragma once

#include <bedrocktools/events/Event.hpp>

#include <algorithm>
#include <chrono>

namespace bedrocktools::events {

struct FrameEvent : Cancellable {
    static constexpr EventType type = EventType::Frame;

    // Request a one-shot hold after the current EGL frame is presented.
    // The game thread is paused only for the requested impact window.
    void requestPostSwapHold(std::chrono::milliseconds duration) noexcept {
        duration = std::clamp(duration, std::chrono::milliseconds(0),
                              std::chrono::milliseconds(750));
        if (duration.count() <= 0) return;

        const auto requestedUntil = std::chrono::steady_clock::now() + duration;
        if (requestedUntil > m_postSwapHoldUntil) {
            m_postSwapHoldUntil = requestedUntil;
        }
    }

    std::chrono::steady_clock::time_point postSwapHoldUntil() const noexcept {
        return m_postSwapHoldUntil;
    }

private:
    std::chrono::steady_clock::time_point m_postSwapHoldUntil{};
};

}
