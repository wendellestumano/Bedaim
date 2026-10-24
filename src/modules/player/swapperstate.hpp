#pragma once

#include <atomic>

// Shared arbitration state for hotbar automation. SwordSwap wins while its
// grounded-target condition is valid; MaceFall may take the slot only after
// ShiftBlade has yielded (normally as the player begins falling).
namespace swapperstate {
inline std::atomic<bool> swordSwapPriority{false};
inline std::atomic<bool> maceFallPriority{false};
}
