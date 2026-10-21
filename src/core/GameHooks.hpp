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
// Queue the normal engine button handler for the next validated LocalPlayer::normalTick, matching
// the engine-tick scheduling used by established Bedrock clients. true means queued, not hit-confirmed.
bool queueAttackInputCallback(void* clientInstance, void* expectedLocalPlayer, void* target);
void cancelQueuedAttackInput();
// Legacy immediate invocation retained for diagnostics only; it does not guarantee an attack or damage.
bool invokeAttackInputCallback(void* clientInstance, void* expectedLocalPlayer);
// The public three-argument route consumes hitPosition; the guarded four-argument route consumes nativeResultOut.
// The four-argument C++ overload preserves existing module call sites; the extended overload exposes the native output flag.
bool attack(void* gameMode, void* target, bool playPredictiveSound, const void* hitPosition);
bool attack(void* gameMode, void* target, bool playPredictiveSound, const void* hitPosition,
            bool* nativeResultOut, bool* nativeResultOutUsed);
// Calls the exact validated public GameMode::useItem route for a player's selected stack.
bool useItem(void* player, const void* itemStack);

// GameType values: Survival=0, Creative=1, Adventure=2, Spectator=3.
bool tryGetPlayerGameType(void* player, std::int32_t& outGameType);

}
