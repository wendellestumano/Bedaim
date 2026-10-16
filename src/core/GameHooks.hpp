#pragma once
#include <cstdint>

namespace bedrocktools::core::gamehooks {

bool install();
void uninstall();
void* clientInstance();
void* gameMode();
// Resolve the local player's owned GameMode instance without waiting for a manual action.
void* gameModeForPlayer(void* player);
// Checks the live GameMode and the exact public attack vtable/signature route without dispatching an attack.
bool attackAvailable(void* gameMode);
bool attack(void* gameMode, void* target, bool playPredictiveSound, const void* hitPosition);
// Test-only probe: queues one native ClientInstance attack-button handler call on the next update.
// A true return means queued, not that damage occurred.
bool queueApollonInputAttackTest();

// Verified LocalPlayer inventory-screen operations resolved only from libminecraftpe.
bool inventoryUiSequenceAvailable();
bool openPlayerInventoryScreen(void* player);
bool closePlayerInventoryScreen(void* player);
// GameType values: Survival=0, Creative=1, Adventure=2, Spectator=3.
bool tryGetPlayerGameType(void* player, std::int32_t& outGameType);

}
