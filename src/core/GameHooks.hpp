#pragma once

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

// Verified LocalPlayer inventory-screen operations resolved only from libminecraftpe.
bool inventoryUiSequenceAvailable();
bool openPlayerInventoryScreen(void* player);
bool closePlayerInventoryScreen(void* player);

}
