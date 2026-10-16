#pragma once

#include <atomic>

// Shared arbitration state: the mace has priority during a confirmed high fall.
// Inline storage makes the same atomic visible to both header-only module users.
namespace swapperstate {
inline std::atomic<bool> maceFallPriority{false};
}
