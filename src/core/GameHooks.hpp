#pragma once
#include <cstddef>
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

// Short-lived /proc/self/maps validation used before version-sensitive native calls.
// This reduces invalid-pointer crashes but cannot guarantee object lifetime across threads.
bool isReadableMemoryRange(const void* pointer, std::size_t size) noexcept;
bool isWritableMemoryRange(const void* pointer, std::size_t size) noexcept;
bool isExecutableMemoryRange(const void* pointer, std::size_t size = 1) noexcept;
bool isValidGameObject(const void* object) noexcept;

// GameType values: Survival=0, Creative=1, Adventure=2, Spectator=3.
// Returns raw ActorGameTypeComponent values: 1=Creative, 5=Default, 6=Spectator.
// The version-specific getter is resolved by a unique ARM64 signature; it accepts
// the Player's EntityContext (Player + mEntityContext), not the Player pointer.
bool tryGetPlayerComponentGameType(void* player, std::int32_t& outComponentValue);
bool tryGetPlayerGameType(void* player, std::int32_t& outGameType);
// Heuristic only: three hitbox-overlapping attack attempts without a hurt timer
// may mark Creative? for targeting, but never overrides an exact native GameType.
bool tryGetPlayerNoDamageCreativeHint(void* player);
// Exact per-actor Creative/Spectator predicates; negative results do not imply Survival.
bool tryGetPlayerCreativeSpectator(void* player, std::int32_t& outGameType);
// Preferred attackability gate matching Actor::isAttackableGamemode used by
// Apollon. Returns false when the current runtime does not expose a trusted symbol.
bool tryIsActorAttackableGamemode(void* actor, bool& outIsAttackable);

}
