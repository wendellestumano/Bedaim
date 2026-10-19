# AutoHit native-context experiment — 2026-10-19

## Baseline recovered from the device
- Current gameplay diagnostics: `/storage/emulated/0/Download/Bedaim/bedaim_diagnostics_auto.json` (100,483 bytes, 413 entries).
- Gameplay-only diagnostics: `/storage/emulated/0/Download/Bedaim/bedaim_diagnostics_gameplay.json` (75,506 bytes, 294 entries).
- Combined history: `/storage/emulated/0/Download/Bedaim/bedaim_diagnostics_total.json` (3,881,398 bytes, 14,644 entries).
- Current log session confirms `ClientInstance::getLocalPlayer` slot 32 repeatedly returns null, then succeeds at sequence 259.
- At sequences 261–264 the runtime reports a guarded four-argument route at slot 2 and installs an observer.
- At sequences 381–385 the route is entered and the trampoline returns true, but the target's `hurtTime` is 0 before and after. This proves native dispatch/return, not damage.
- Target selection also repeatedly alternates between a valid player and no eligible actor, so target freshness remains an independent gate.

## Apollon source and APK analysis
- Source repository: https://github.com/TheChuan1503/apollon-client-for-minecraft-pe
- In `main.cpp`, `emulateNativeAttack` uses a pending flag and calls `ClientInstance::handleDestoryOrAttackButtonPress()` during `ClientInstance_onTick`. This is an engine input-callback route, distinct from directly calling `GameMode::attack`.
- The source's hard-coded offsets are stale for the analyzed `libminecraftpe.so`: address `0xCCC075C` disassembles inside `Java_com_mojang_minecraftpe_AppExitInfoHelper_nativeSendApplicationExitInfo`, and `0x6462B60` contains non-code data. Do not copy these offsets into Bedaim.
- The analyzed library at `Testing2/extracted/minecraft/libminecraftpe.so` is 328,419,680 bytes. A candidate four-argument function body reads from the fourth argument at offsets beyond `0xAD`; the current Bedaim caller was passing the address of a 12-byte `Vec3`. This makes the old `hitPosition` assumption unsafe. The exact native context type is not claimed as fully proven from this alone.

## Experimental source change
Branch: `perf/autohit-tick-throttle-20261019`

- Replaced the synthetic `&hitPoint` fourth argument with the engine-owned `Level::storedHitResult()` context.
- Resolve `HitResultGetEntity` through the BedrockTools signature registry and verify the resolved function belongs to `libminecraftpe.so`.
- Require the engine HitResult's actor pointer to equal the selected Aim Assist target before dispatch; mismatches are logged and skipped.
- Use the actual engine HitResult position for the range check.
- Renamed the API parameter from `hitPosition` to `attackContext` to avoid preserving the unverified type assumption.
- Added a pure `hitResultMatchesTarget` helper and regression tests for equal, mismatched, and null targets.
- Source commits, in order:
  - `b5e834622832e5bd778d01a4591261264130f91a` — guard declaration
  - `6a0c7f9090771034b394f60a62583727157ade46` — guard implementation
  - `8692451a3336589da509f5e95f35b96ecb431c89` — tests
  - `26ea526063e1f103871b7a4fffef7a2f0f1eef36` — AutoHit uses engine HitResult context
  - `5385d1d9de291faf127ac7a2ae5e1d90c7f9c033` — API declaration comment
  - `3bef08891f4586df100fdbf21e9d929964637a0a` — GameHooks parameter rename
- `main` was not modified.

## Status / test ledger
| Date | Branch / commit | Static tests/build | In-game damage | Lag/regression |
|---|---|---|---|---|
| 2026-10-19 | `perf/autohit-tick-throttle-20261019`, code commit `3bef08891f4586df100fdbf21e9d929964637a0a` | Termux xmake host test build started; result pending | Not tested after patch | Not tested after patch |

## Interpretation and next steps
- This is a targeted experiment, not a verified fix. The main hypothesis is that a synthetic `Vec3*` was being passed where the four-argument native route expects an engine-owned context object.
- If the new diagnostic reports `native_hit_result_target_mismatch`, the next problem is target/rotation synchronization, not native function return handling.
- If the accessor cannot be resolved, do not replace it with a hard-coded offset; inspect the exact target build and resolve the accessor safely.
- Run all host regression tests and Android arm64 build. Then test in a private world against a known damageable target and verify actual health change, correct target identity, and no lag. Do not claim success from particles or a true return.
