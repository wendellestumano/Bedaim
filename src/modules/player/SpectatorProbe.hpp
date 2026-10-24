#pragma once

namespace bedaim::spectatorprobe {

// Conservative, multi-sample heuristic used internally by Aim Assist.
// This is a helper, not a launcher-visible module.
bool isSpectatorCandidate(void* actor);

} // namespace bedaim::spectatorprobe
