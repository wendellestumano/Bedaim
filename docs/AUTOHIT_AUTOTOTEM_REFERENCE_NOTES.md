# AutoHit / AutoTotem reference research

## Scope

This note records what was actually available in the supplied archives and how it informs Bedaim. The references are used to compare API patterns and diagnostic design; their code is not copied wholesale into the native module.

## Official Minecraft sample scripts

The supplied `minecraft-samples-main.zip` contains TypeScript examples using the public Bedrock Script API:

- `casual_creator/gray_wave/scripts/GrayWaveManager.ts` subscribes to `world.afterEvents.itemUse`, reads the player's `minecraft:inventory` component, and works with its `container`.
- `creator_camp/scripts/main.ts` reads the selected hotbar slot using `player.selectedSlotIndex` and `container.getItem(...)`.
- `creator_camp/scripts/CommonUtils.ts` demonstrates writing a changed item stack back with `container.setItem(...)`.
- `debug_tools/scripts/tools/GameModeWatchTool.ts` uses the public `player.getGameMode()` API to report a player's game mode.

These examples support three design choices for Bedaim:
1. Treat input/use events as observations, not proof that an attack or inventory operation succeeded.
2. Keep inventory reads, decisions, and writes separate, and verify state after a write.
3. Log the first blocking condition rather than emitting the same per-tick message repeatedly.

The supplied `bedrock-samples-main.zip` has resource/behavior JSON samples but no JavaScript/TypeScript script files. The sample scripts do not provide a native `GameMode::attack` implementation or a native offhand-slot setter, so they cannot validate those C++ entry points.

## Apollon archive inspection

The supplied `Apollon_code.zip` contains Android shared libraries (`.so`) rather than readable source files. The relevant client library is stripped; the archive does not provide a source implementation that can safely be adapted or attributed line-by-line. No unverified offsets or native calls were copied from it.

## Current findings from Bedaim diagnostics

The latest supplied diagnostics export recorded:

- `GameHooks`: the public attack signature was found, but the survival attack signature and the internal attack hook were not available in that runtime.
- `AutoHit`: `gameMode=unavailable`; no synthetic attack should be dispatched until the live GameMode object and its public attack entry are validated.
- `AutoTotem`: the offhand read stopped before attempting a transfer because the Player pointer was reported unreadable. This is distinct from an empty offhand and must never be treated as permission to move an item.
- The diagnostics autosave itself successfully exported the rolling JSON file.

The native code now adds a local-player identity filter around the generic `NormalTick` hook and emits more specific GameMode-resolution and Player-pointer diagnostics. This is intended to distinguish a non-local/stale actor from a valid local player whose native GameMode/offhand API is unavailable.

## Validation boundary

Host regression tests and an Android ARM64 package build verify compilation and pure policy behavior only. AutoHit and AutoTotem remain experimental until a fresh in-game diagnostics export shows a valid local-player tick, a verified attack route, and a readable offhand stack, followed by controlled manual verification in Minecraft Bedrock/Levi.

## Follow-up: 2026-10-17 local-player tick regression

The field diagnostics export after Build #618 records that:
- `ClientInstance::getLocalPlayer` resolution is unavailable.
- The `NormalTick` detour consequently suppresses `LocalPlayerPreTickEvent` and `LocalPlayerTickEvent`.
- Aim Assist and Hitbox Expander were enabled and their native dependency probes reported ready, but those probes only show that target-query/camera functions resolved; they do not indicate that their per-player tick callbacks are running.
- GameMode public-attack signature resolution is present, but SurvivalMode/internal attack signatures and attack hooks are unavailable. `UseItemAsAttack` hooks are also unavailable in this runtime.

The regression was introduced by the local-player identity filter in commit `12a9d6f13fa5a7089accbc03c4f6575de9dd77f8`: it correctly avoids treating arbitrary actors as the local player, but the only local-player identity source is a signature that fails on this runtime. Do not remove the filter or substitute a guessed vtable slot. The follow-up branch improves diagnostics to distinguish an unresolved getter signature from a resolved getter returning null. A validated alternate local-player source is still required before re-enabling gameplay tick events.

AutoHit's existing target path already checks the Aim Assist target snapshot and whether the target bounds intersect the crosshair. The requested immediate-on-crosshair behavior must retain the per-weapon CPS cap, fresh-target checks, and validated native attack gate. No runtime attack route was established by this diagnostics export, so immediate dispatch remains blocked until the native route is validated.
