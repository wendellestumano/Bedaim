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