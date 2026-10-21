#pragma once
#include <cstdint>

namespace bedrocktools::core::gamehooks {

bool install();
void uninstall();
void* clientInstance();
// Returns the validated ClientInstance-owned local player, or nullptr if unresolved.
void* localPlayer();
void* gameMode();
// Resolve the local player's owned GameMode instance without waiting for a manual action.
void* gameModeForPlayer(void* player);
// Checks the live GameMode and the exact public attack vtable/signature route without dispatching an attack.
bool attackAvailable(void* gameMode);
// Uses only an exact exported ClientInputCallbacks symbol and validates that the receiver is the current ClientInstance owning expectedLocalPlayer.
bool inputAttackCallbackAvailable(void* clientInstance, void* expectedLocalPlayer);
// Invokes the normal engine attack-button callback; true means invoked, not that an attack or damage was confirmed.
bool invokeAttackInputCallback(void* clientInstance, void* expectedLocalPlayer);
// Both validated routes consume hitPosition. The optional outputs report route acceptance
// and whether the selected native ABI has four arguments; neither proves server damage.
bool attack(void* gameMode, void* target, bool playPredictiveSound, const void* hitPosition);
bool attack(void* gameMode, void* target, bool playPredictiveSound, const void* hitPosition,
            bool* routeAcceptedOut, bool* fourArgumentRouteUsed);
// Calls the exact validated public GameMode::useItem route for a player's selected stack.
bool useItem(void* player, const void* itemStack);

// GameType values: Survival=0, Creative=1, Adventure=2, Spectator=3.
bool tryGetPlayerGameType(void* player, std::int32_t& outGameType);

}
