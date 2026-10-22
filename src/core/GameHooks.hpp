#pragma once
#include <cstdint>

namespace bedrocktools::core::gamehooks {

bool install();
void uninstall();
void* clientInstance();
// Coherent ClientInstance identity snapshot. Generation changes on each observed pointer transition.
bool clientInstanceSnapshot(void*& outInstance, std::uint64_t& outGeneration);
void* gameMode();
// Resolve the local player's owned GameMode instance without waiting for a manual action.
void* gameModeForPlayer(void* player);
// Checks the live GameMode and the exact public attack vtable/signature route without dispatching an attack.
bool attackAvailable(void* gameMode);
bool attack(void* gameMode, void* target, bool playPredictiveSound, const void* hitPosition);

// GameType values: Survival=0, Creative=1, Adventure=2, Spectator=3.
bool tryGetPlayerGameType(void* player, std::int32_t& outGameType);
// Preferred attackability gate matching Actor::isAttackableGamemode used by
// Apollon. Returns false when the current runtime does not expose a trusted symbol.
bool tryIsActorAttackableGamemode(void* actor, bool& outIsAttackable);

}
