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

// GameType values: Survival=0, Creative=1, Adventure=2, Spectator=3.
bool tryGetPlayerGameType(void* player, std::int32_t& outGameType);
// Exact per-actor predicates: reports Creative=1 / Spectator=3 when confirmed,
// or mode=0 only when both predicates are callable and both are false.
bool tryGetPlayerCreativeSpectator(void* player, std::int32_t& outGameType);
// Coarse fallback used by Apollon: false means the actor is not attackable in its
// current gamemode. This does not distinguish Creative from Spectator.
bool tryGetPlayerAttackableGamemode(void* player, bool& outAttackable);

}
