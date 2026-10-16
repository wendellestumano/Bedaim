#pragma once
#include <cstdint>

namespace bedrocktools::core::gamehooks {

bool install();
void uninstall();
void* clientInstance();
void* gameMode();
// Resolve the local player's owned GameMode instance without waiting for a manual action.
void* gameModeForPlayer(void* player);
// Checks whether the isolated build can submit an attack through the symbol-resolved
// ClientInstance input handler or the exact public GameMode wrapper. Does not dispatch.
bool attackAvailable(void* gameMode);
// Submits one attack request; true means accepted/queued, not confirmed damage.
bool attack(void* gameMode, void* target, bool playPredictiveSound, const void* hitPosition);

// Verified LocalPlayer inventory-screen operations resolved only from libminecraftpe.
bool inventoryUiSequenceAvailable();
bool openPlayerInventoryScreen(void* player);
bool closePlayerInventoryScreen(void* player);
// GameType values: Survival=0, Creative=1, Adventure=2, Spectator=3.
bool tryGetPlayerGameType(void* player, std::int32_t& outGameType);

}
