#pragma once

namespace bedaim::autototem {

enum class Decision {
    Idle,
    WaitForDelay,
    WaitForCombat,
    WaitForContainer,
    WaitForItem,
    WaitForOffhandClear,
    Equip
};

struct Inputs {
    bool enabled;
    bool pending;
    bool offhandHasTotem;
    bool delayElapsed;
    bool combatOnly;
    bool inCombat;
    bool containerOpen;
    bool candidateAvailable;
    bool offhandEmpty;
};

// Pure decision policy only. The caller must perform and validate the native
// inventory transfer separately; this function never touches Minecraft memory.
Decision decide(const Inputs& input) noexcept;
float normalizeDelayMs(float value) noexcept;
float normalizeScanRetryMs(float value) noexcept;
float normalizeOffhandPollMs(float value) noexcept;
float normalizeUiStepDelayMs(float value) noexcept;

} // namespace bedaim::autototem
