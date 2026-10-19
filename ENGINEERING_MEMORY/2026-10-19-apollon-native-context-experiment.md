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
- The analyzed library at `Testing2/extracted/minecraft/libminecraftpe.so` is 328,419,680 bytes. Re-inspecting the candidate at `0xF89FE18` corrected an earlier mistaken read: x3 is saved as x21 and the function writes a byte through `[x21]` on several result paths (`strb wzr,[x21]`, then writes to `[x21]` again). This is evidence that the guarded four-argument route's fourth argument is a writable output flag, not a `Vec3*` or `HitResult*`. The earlier HitResult-context experiment was superseded and its code was reverted.

## Experimental source change
Branch: `perf/autohit-tick-throttle-20261019`

- First attempt (commits `26ea526...` through `3bef088...`) replaced `&hitPoint` with `Level::storedHitResult()`. It was superseded after re-reading the disassembly: x3 is written as an output flag, so a HitResult pointer would corrupt engine state. The code and temporary tests for that hypothesis were reverted.
- Current experiment passes a writable `bool*` output flag to the guarded four-argument route, while preserving `&hitPoint` only for the separately validated public three-argument route.
- `GameHooks::attack` distinguishes the two ABIs and reports whether the native out-parameter was applicable; AutoHit logs the native function return separately from the output flag and hurt-time observation.
- A compatibility overload preserves existing four-argument C++ call sites in Mace Swapper and Elytra Macing; the extended overload is used by AutoHit to observe the out flag.
- `AttackEvent` keeps its hit-position field separate from the native output pointer. Native/manual detour events leave that position field null rather than mislabeling the output flag as a vector.
- The output flag is not treated as proof of damage. A private-world health-change test is still required.
- Source commits, in order:
  - `b5e834622832e5bd778d01a4591261264130f91a` — guard declaration
  - `6a0c7f9090771034b394f60a62583727157ade46` — guard implementation
  - `8692451a3336589da509f5e95f35b96ecb431c89` — tests
  - `26ea526063e1f103871b7a4fffef7a2f0f1eef36` — AutoHit uses engine HitResult context
  - `5385d1d9de291faf127ac7a2ae5e1d90c7f9c033` — API declaration comment
  - `3bef08891f4586df100fdbf21e9d929964637a0a` — superseded GameHooks parameter rename
  - `44075bf5de22d3dd27dcaf5d897ec6a9e58300cf` — compatibility overload for existing module call sites
  - `b6e5c33a29d053b7e98c49c839d5d01de34dd029` — final event argument separation; this is the source commit built by workflow run `39253430636`
- `main` was not modified.

## Status / test ledger
| Date | Branch / commit | Static tests/build | In-game damage | Lag/regression |
|---|---|---|---|---|
| 2026-10-19 | `perf/autohit-tick-throttle-20261019`, `b6e5c33a29d053b7e98c49c839d5d01de34dd029` | Host regression tests passed; Android arm64 build and package succeeded in workflow run [#891](https://github.com/wendellestumano/Bedaim/actions/runs/39253430636). Artifact: `BedrockTools-levipack`, 680,016 bytes, SHA-256 `d74b136439a27725c7c2deab127ac445d2e0aeb7b6dc0db992de72b991efd2c9`. | Not tested after corrected patch | Not tested after corrected patch |

## Interpretation and next steps
- This remains an ABI hypothesis based on the observed byte writes through x3; confirm against the exact live function and current build before declaring the signature proven.
- Preserve separate telemetry for `GameMode` function return, the native out-flag, and target health/hurt-time. None individually proves server damage.
- Next, test this build in a private world against a known damageable target. Record `native_out_result`, function return, target health/hurt-time, target identity, and lag. If the output flag is false or damage is still absent, investigate the Apollon-style `ClientInstance::handleDestoryOrAttackButtonPress()` callback as a separate fallback; do not copy its stale offsets. Do not claim success from particles or a true return.
