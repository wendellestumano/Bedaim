# Recovery audit — 2026-10-17

## Scope and safety

This branch is a recovery candidate only. Do not merge or install without CI and in-game validation. The main branch is not modified.

## Verified baseline

The intact baseline for `src/core/GameHooks.cpp` is blob `be2cd3fe628a40cf55670b458748d61855928d2e` from `fix/local-player-tick-diagnostics-20261017`. The damaged experimental branch `fix/local-player-getter-vtable-fallback-20261017` must not be used as source of truth for GameHooks.

## Findings

- Existing logs report `ClientInstance::getLocalPlayer` returning null; the strict identity gate suppresses LocalPlayerPreTick/LocalPlayerTick events. This prevents Aim Assist and Hitbox Expander processing and also prevents AutoHit/AutoTotem from receiving player ticks.
- A vtable fallback is runtime/ABI sensitive. Readable memory and code-image validation do not alone prove the slot has the right semantics on the exact Minecraft build.
- The AutoTotem offhand-reader quarantines an invalid getter. It must be diagnosed separately after local-player ticks resume.
- The Module Diagnostics overlay showed only counts/registry summaries, even though the diagnostics buffer stores actual Entry records. It needs to render recent Warning/Error/Critical entries as well as counts.
- The automatic log export should remain on; preserve its periodic worker, atomic writer usage, export format and saved path/status.

## Acceptance criteria

1. `GameHooks.cpp` is restored byte-for-byte from the intact baseline before any further native changes.
2. Overlay renders actual runtime warning/error entries and clearly distinguishes runtime log counts from registry audit counts.
3. All host regression tests and Android ARM64 build pass.
4. In-game `GameHooks/local_player_resolution` reaches `ready` or `ready_vtable_fallback`; local player tick events arrive.
5. Aim Assist and Hitbox Expander demonstrably process frames/ticks.
6. AutoTotem logs a validated offhand identifier and reports transfer result plus verified postconditions; no unsafe setter is used.
7. AutoHit logs validated public attack route and a real native attack result.
8. Module Diagnostics automatic log export remains working.

## Limitations

A green native build cannot establish runtime hook ABI or in-game behavior. These require fresh logs from the exact game/runtime version.
