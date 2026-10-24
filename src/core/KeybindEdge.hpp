#pragma once
#include <atomic>

namespace bedrocktools::core {

// Converts repeated key-down events into a single press edge. A key-up always
// releases the latch, including when it arrives while the menu blocks input.
class KeybindEdge {
public:
    bool update(bool isDown) noexcept {
        if (!isDown) {
            held_.store(false, std::memory_order_release);
            return false;
        }
        return !held_.exchange(true, std::memory_order_acq_rel);
    }

    void reset() noexcept {
        held_.store(false, std::memory_order_release);
    }

private:
    std::atomic_bool held_{false};
};
} // namespace bedrocktools::core