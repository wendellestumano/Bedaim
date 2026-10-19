# 2026-10-19 — Current build, Apollon, and guarded attack thunk

## Exact source and branch
- Repository: `wendellestumano/Bedaim`
- Branch: `perf/autohit-tick-throttle-20261019`
- Source code change: `49543f9c3b6b3cc9e561f8072bf5e4d00f7f111d` — decouples AutoHit target/rotation tracking from native dispatch route availability.
- Build source commit: `e86eb4d7583a45d88b6ad5ca0c2eb6cf5b54957b`. This commit adds engineering notes after the code change; gameplay code is the same.
- `main` was not modified.

## Current experimental build and automated validation
- GitHub Actions run: https://github.com/wendellestumano/Bedaim/actions/runs/39248857505
- Run completed successfully. Steps marked successful: host regression tests, NDK r28c setup, Android build/package, artifact upload.
- Artifact ID: `12184054273`, name `BedrockTools-levipack`, artifact ZIP SHA-256 `be7d27e741cb9c86b0cf0ded3cab5e2854fe7556237bd770b37b1858d844f1d6`.
- Inner `BedrockTools.levipack` SHA-256: `bbff3889909ec3e7368630a230eb79eb87913903791dca708e36015a84c1b385`; 681,146 bytes. Manifest version is 1.5.7.
- Download delivered in this work as `Bedaim-AutoHit-tracking-fix-e86eb4d.levipack.zip`, containing `BedrockTools.levipack`.
- Exact current source archive extracted to an isolated testing copy under Testing2. Excluded two malformed ZIP entries named `-la` and `sist target stability and limits"`; neither was extracted or executed.
- Full local host regression test suite run from that exact source copy exited 0:
  - `menu_config_value_test`: 64 checks, 0 failures.
  - `aimassist_math_test`: 27 checks, 0 failures.
  - `atomic_config_writer_test`: 10 checks, 0 failures.
  - `diagnostics_test`: exit 0, no check-count output.
  - `autohit_guards_test`: 42 checks, 0 failures.
  - `autohit_weapon_profile_test`: exit 0, no check-count output.
  - `autototem_policy_test`: 14 checks, 0 failures.
- These results establish build/test success only. No in-game gameplay, actual damage, or no-lag test was performed.

## Apollon source review
- Public source: https://github.com/TheChuan1503/apollon-client-for-minecraft-pe
- In its `emulateNativeAttack` path, KillAura sets `doHandleAttack`, then a `ClientInstance_onTick` hook consumes that flag using `handleDestoryOrAttackButtonPress()` and clears it. This is the most useful architectural lead because it delegates to the engine's normal attack-input handler instead of immediately calling GameMode attack.
- Existing Apollon offsets `Attack=0xCCC075C`, `NormalTick=0x6BEDA24`, and `HandleDestroyOrAttack=0x6462B60` are not transferable to Minecraft 1.26.52.3. For example, `0xCCC075C` disassembles inside `Java_com_mojang_minecraftpe_AppExitInfoHelper_nativeSendApplicationExitInfo`, and `0x6462B60` lies below the target binary's executable `.text` range. Do not copy these offsets.
- Public source license was not found at the extracted repository root in this pass; keep source as a reference, do not wholesale copy it.

## Runtime-binary cross-reference
- Target: extracted `libminecraftpe.so` from Minecraft 1.26.52.3 ARM64. SHA-256: `53c4a86a8f3dbd7f0d48e440f8607d63c55100aeb364c80d19afefa8097e233a`.
- Historical runtime diagnostic showed GameMode vtable slot 2 at runtime address `0x7a4a8ea048`, the candidate attack implementation at `0x7a4a8e5e18`, slot 16 at `0x7a4a8e9fe0`, and image base `0x7a3b046000` (addresses normalized from those logs).
- Relative offsets: slot 2 `0xF8A4048`; attack candidate `0xF89FE18`; slot 16 `0xF8A3FE0`.
- Target disassembly of `0xF8A4048`: begins `ldrb w8,[x0,#0xc8]`, has conditional/CBZ guards, an early path that returns false, and an unconditional `B 0xF89FE18` on the alternate path.
- This is the guarded four-argument thunk shape recognized by the current GitHub `isGuardedFourArgumentAttackThunk` implementation (first instruction `0x39432008`, conditional guard, alternate return, direct tail branch to the resolved attack signature). Slot 16 instead branches to `0xF89FA80`, not the attack candidate.
- The attack implementation at `0xF89FE18` includes paths explicitly setting `w0` to 0 or 1 before returning, consistent with a boolean result. This is static ABI evidence, not evidence that the server applied damage.
- The older Build #844 logs use the older `GameHooks.cpp` snapshot whose failure log says `No exact public GameMode::attack vtable match`. The current branch has a newer second-phase guarded-thunk validator, so Build #844 results should not be used to infer whether this current validator succeeds.
- Key expectation for next in-game log: `four_argument_native_attack_validated` or `validated guarded four-argument attack thunk`. If absent, the live GameMode object/vtable differs from the historical object and needs to be inspected; do not widen the validator blindly.

## Current conclusion
- Confirmed functional improvement: target acquisition/rotation no longer shuts off solely because native attack-route validation fails.
- Confirmed build/test result: latest experimental branch built and packaged successfully; all seven host regression targets completed successfully.
- Open: identify the live handler's route in the current session, confirm local-player and target stability, confirm actual target health/damage feedback, then test lag. The AutoHit is not yet declared fixed.
