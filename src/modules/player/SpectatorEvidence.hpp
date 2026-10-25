#pragma once

namespace bedaim::player {

// Keeps Aim Assist's conservative fallback when native game-mode APIs are unavailable.
// This is an internal utility, not a registered module or player-facing overlay.
void initializeSpectatorEvidence();
bool isSpectatorCandidate(void* actor);

} // namespace bedaim::player
