# AutoHit deep-diagnostics pass — local APK reference (2026-10-19)

## Source of truth for the Apollon comparison

Use only the user's local APK `/storage/emulated/0/Download/Testing2/MCPE_Apollon_Client_1.26.52_v6.7_64Bit.apk` and its matching extracted libraries under `Testing2/extracted/apollon/`. Do not use the old public Apollon repository/source or carry over offsets from that version.

Verified matching APK-contained/extracted SHA-256 values:
- `libReverseSigma.so`: `b132fd124e26c05656d7a9ef53769ed20923fda4c37fed712a849e557088ac74`
- `libmaesdk.so`: `752fbee0914bfbf223aa61b386823f88b2e622d0783f8fd6b68790e1ddcf8f1f`
- `libminecraftpe.so`: `53c4a86a8f3dbd7f0d48e440f8607d63c55100aeb364c80d19afefa8097e233a`

`libReverseSigma.so` contains strings `shadowhook version 2.0.1`, `emulateNativeAttack`, `killAura`, `KillAura`, `minKillAuraCPS`, `maxKillAuraCPS`, and `hook_func_addr`. This establishes that the local APK bundles ShadowHook and has an emulateNativeAttack setting; it does not by itself prove which function is called or what its ABI is. String xrefs around file virtual addresses `0x99174`, `0x9941c`, `0x996f4`, `0x9fa78`, `0x9fe60`, `0xa04ac`, and `0xd2f38` include configuration-string/metadata access and must not be mistaken for attack-function entry points without control-flow context.

ShadowHook is a hook backend, not a substitute for identifying the correct method. Do not add it to Bedaim just because it is present in the APK. First validate the current `GameMode` function, the internal observer, or a client input callback from the same APK/build, including function boundary, AArch64 ABI, lifetime and thread/reentrancy behavior. Do not patch a guessed address.

## Actual Bedaim session inspected

Files read directly from the connected phone at `/storage/emulated/0/Download/Bedaim/`: `bedaim_diagnostics_auto.json` and `bedaim_diagnostics_gameplay.json`, updated 2026-10-19 around 11:30 local time. The auto export contains 548 records (schema v1, 4096-entry ring); gameplay contains 437 records.

Observed counts and decisive samples:
- 86 local-player getter/slot-32-null reports and 86 `verified_local_player=resolution_failed` messages.
- GameHooks validated a four-argument guarded attack thunk at vtable slot 2 and reported observer installation on the resolved target.
- 17 `native_attack_accepted` probe entries, 16 with `native_out_result=false`; the same 16 had `target_hurt_time_before=0` and `target_hurt_time_after=0`.
- First logged accepted dispatch was around distance 2.106 blocks, range 3.0, sword 4 CPS; later attempts included distances below 1 block. Therefore reach/CPS do not explain the absent hits by themselves.
- Target tracking flapped: 23 `new target selected`, 23 `target_selection=none`, 9 fresh target snapshots and 10 stale/missing snapshots in this short export.
- HitboxExpander emitted `actor_snapshot=malformed` once, then `actor_snapshot=valid` once. The old schema omits the vector pointers/span/stride details, so that malformed vector's exact reason cannot be recovered retroactively.
- `GameType` getter/setter-cache unavailable was warned once. It may affect mode-aware filters, but this is not proven to be the immediate native dispatch cause; do not default Unknown to Survival or guess the offset.
- AutoTotem `tick_gate=disabled` is an expected disabled-module state and is being changed to be silent at tick level, with one lifecycle info event instead of a warning.

## Code changes included in this pass

- Bounded ring capacity rises from 4,096 to 32,768 entries (8x), while tracking dropped/overwritten count.
- Every diagnostics entry gains monotonic nanoseconds and a thread identifier for correlating events sharing the same millisecond. ALL and gameplay-only exports expose buffer capacity, dropped count and sequence ranges; health panel reports the same metadata.
- AutoHit logs a begin and result record for every actual dispatch, including attempt ID, player/GameMode/target pointers, weapon/CPS, eye/contact point/target bounds, distance/range, native call duration, wrapper/observer result, out flag, and hurtTime before/after. `reportGate` remains state-deduplicated, while per-attempt telemetry is not deduplicated.
- GameHooks validates that the observer is installed on the exact implementation it intends to observe and clears result flags before every synthetic dispatch so a call cannot inherit stale `observed=true` or stale native-return state from the previous attempt. Dispatch telemetry separates public return, internal native return, observer-install/readiness/reach, output flag and vtable slot/ABI.
- AimAssist and HitboxExpander share stricter actor-vector metadata validation. Canonical empty vectors are recorded as empty, not malformed; genuinely malformed vectors report a reason (`null_begin`, `null_end`, `null_capacity`, `end_before_begin`, `capacity_before_end`, stride alignment, or count/capacity limits) with addresses and sizes, without dereferencing invalid elements.
- AutoTotem no longer emits a warning every tick while disabled. It emits lifecycle info when disabled and reserves warnings for a missing player while enabled.

## Remaining unknown / next experiment

The current logs show a route validation and hook observer but do not show successful server-authoritative damage. The out flag remains false and hurtTime unchanged; these are separate signals and neither should be mislabeled as damage. This pass deliberately does not add an artificial animation or call an unvalidated native `handleDestoryOrAttackButtonPress` address. Next, use the new `native_attack_dispatch_result` and `attack_attempt_begin/result` records, then finish identifying the exact `emulateNativeAttack` control-flow path in the local APK before wiring the normal game input callback/animation. Current Bedaim dispatch label remains `GameMode_vtable`, explicit input callback not called.

Only `perf/autohit-tick-throttle-20261019` is modified. Keep `main` untouched. Build and runtime testing remain separate: a successful build is not evidence that AutoHit landed damage.