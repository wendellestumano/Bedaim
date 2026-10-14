#pragma once

namespace bedrocktools::core::gamehooks {

bool install();
void uninstall();
void* clientInstance();
void* gameMode();
// Resolve the local player's owned GameMode instance without waiting for a manual action.
void* gameModeForPlayer(void* player);
bool attack(void* gameMode, void* target, bool playPredictiveSound, const void* hitPosition);

}
