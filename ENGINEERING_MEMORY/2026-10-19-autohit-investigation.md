# AutoHit investigation — 2026-10-19

## Scope and constraints
- Repository: `wendellestumano/Bedaim`
- Working branch: `perf/autohit-tick-throttle-20261019` (confirmed to exist via GitHub branch search).
- `main` must remain untouched.
- User reports Build #844 compiled, but in-game AutoHit emits particles, stops looking at the target, and causes no damage.
- This note documents evidence recovered during this investigation; no source-code changes or successful gameplay fix are claimed here.

## Retrieved artifacts
- `bedaim_diagnostics_auto(20261019-101858).json` — Library file ID `file_00000000a5b0820e999cc92a0d667efc`, 229,756 bytes.
- `bedaim_diagnostics_auto(20261019-101844).json` — Library file ID `file_000000003898820ea2534c75529e293c`, 229,756 bytes.
- `bedaim_diagnostics_gameplay(20261019-095420).json` — Library file ID `file_000000000b6c820e866f4635df098976`, 114,808 bytes.
- `bedaim_diagnostics_total(20261019-095421).json` — Library file ID `file_0000000089e8820e8671a3bea7d4f918`, 3,502,137 bytes.
- APKs / prior videos: not yet verified accessible in this investigation. Do not assume Termux or Testing2 is remotely accessible through the current tools.

## Confirmed log evidence
1. Runtime logs state no exported exact `ClientInstance::getLocalPlayer` symbol was found; fallback vtable getter slot 32 returns null repeatedly, followed by `verified_local_player=resolution_failed`.
2. Hook initialization says `NormalTick` installed, ClientInstance update hook installed by signature pattern, and the public attack observer is deferred until a live public three-argument route is validated.
3. Logs report `No exact public GameMode::attack vtable match` and `No validated public 3-argument attack route`. This means dispatch should not be assumed available on this runtime.
4. Some older gameplay logs show `native_attack_accepted` while target `hurtTime` remains 0 before and after. This is not authoritative evidence of server damage.
5. A later autosnapshot contains `target_quarantined_no_hit_feedback` after six accepted attempts, while the currently fetched source text describes continuing at configured CPS after six no-feedback attempts. This is a source/log behavior discrepancy to investigate against the exact artifact commit before changing the guard.
6. A 5-second native-route probe backoff is present in current `src/modules/player/autohit.cpp`; this throttles repeated failed route discovery but cannot repair an invalid route by itself.
7. Current source resolves `gameModeForPlayer(player)`, requires `attackAvailable(gameMode)`, then uses AimAssist's fresh target snapshot and performs further target/range checks. Thus local-player identity and route validation are upstream gates, not merely a CPS issue.

## Current source inspected
- `src/core/GameHooks.cpp` on the experimental branch.
- `src/modules/player/autohit.cpp` on the experimental branch.
- `src/modules/player/aimassist.cpp` on the experimental branch.
- Source fetch returned file content, but the branch HEAD SHA has not yet been pinned in this note. Record it before any code modification or cherry-pick.
- No code changes made during this investigation; no tests/builds executed here.

## ProtoHax documentation research
Primary docs:
- https://userscript.protohax.net/api/local-player/
- https://userscript.protohax.net/api/combat-helpers/
- https://userscript.protohax.net/api/entities/
- Repository: https://github.com/hax0r31337/ProtoHax

Relevant documented architecture:
- Local player is exposed through session state (`ctx.session.entityState.localPlayer`), with `isInitialized` and a tick counter.
- `interactEntity(entity, swing, rotation, "attack")` queues an attack/interact action; `click(rotation)` queues a click.
- Input data is a list of auth-input tags and is only meaningful during the documented tick event. This is not equivalent to reading the Android/Levi Launcher physical attack key.
- Combat Helpers separates target selection (`getTargets` and target filters), click scheduling/CPS, and rotation scheduling. Rotation requested during `movement_tick` is committed for that tick's auth-input packet.
- ProtoHax's documented attack action is tied to its own session/network/auth-input pipeline. It is a useful behavioral reference, not a drop-in replacement for Bedaim's native C++ `GameMode::attack` route.

## Hypotheses — not yet proven
- The persistent null from vtable slot 32 may indicate the chosen receiver/vtable slot/ABI is wrong for this runtime, or the receiver is not the expected ClientInstance subtype/state. Do not simply change slot numbers without evidence from exact symbols, disassembly, and known-good receiver validation.
- The attack route may be incorrectly inferred from signatures/vtable shape or have an ABI mismatch. A positive wrapper return may only mean a native function returned true, not that a valid server attack was processed.
- Particle feedback may be cosmetic/swing feedback detached from accepted damage.
- The user's report that the camera stops tracking may be a side effect of the target provider losing a fresh local-player identity, but this causal link remains unverified.

## Required next steps
1. Pin the exact branch HEAD SHA and Build #844 source commit; compare the source against the diagnostic snapshot's reported behavior.
2. Read the entire local-player resolver and public attack-route validator, then trace the receiver provenance for ClientInstance and GameMode. Do not guess offsets or slots.
3. Add/verify stage-specific telemetry: local player resolved and identity stable; target acquired and still live; rotation request committed; attack route exact match + ABI; dispatch entered; native result; delayed target/server damage feedback if observable.
4. Build a controlled, low-rate test in a private world/server with a known damageable target; compare a manual attack baseline against AutoHit. Confirm actual health/damage change separately from particles, swing, and return values.
5. Investigate input binding as an independent optional activation mechanism only after confirming how Levi Launcher exposes keybinds. It cannot fix native dispatch.
6. Retrieve the APKs/video only if the corresponding files are actually available through the Library, current conversation, or an authorized remote desktop connection.
7. Run automated tests and build after each source change; log exact result and artifact URL/SHA here.

## Test ledger
| Date | Branch / commit | Change | Static tests/build | In-game damage | Lag/regression |
|---|---|---|---|---|---|
| 2026-10-19 | `perf/autohit-tick-throttle-20261019`; exact HEAD SHA pending | Evidence recovery and documentation only | Not run in this investigation | Not confirmed | Not tested |
