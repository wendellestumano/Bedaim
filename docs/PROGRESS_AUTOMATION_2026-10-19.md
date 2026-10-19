# Bedaim Dev Shift — 2026-10-19

## Scope and safety
- Repository: `wendellestumano/Bedaim`.
- `main` was not changed or merged.
- Existing local checkout `~/Bedaim-working-20261019` stayed on `perf/autohit-tick-throttle-20261019` and was clean before validation; it was not switched or overwritten.
- Validation used a separate worktree at `~/Bedaim-validation-20261019`, tested at PR head `28a734c4b18216c13322ac1e248daa525fc4af61`.

## Evidence and hypothesis
- Build #901 failed during Android ARM64 `Build and package`, after host regression tests passed.
- Failure: `src/core/GameHooks.cpp:2080:55: error: too many arguments to function call, expected 3, have 4`.
- `src/core/Diagnostics.hpp` declares `diagnostics::log(Level, component, message)` with three arguments. The fix combines `native_attack_dispatch_result` and `dispatchResult` into one message string.
- Code commit: `a9e4b0f8ffe87b787a3916173ddc6bdf616dc523`.
- PR #41: https://github.com/wendellestumano/Bedaim/pull/41 — draft/open, based on `perf/autohit-tick-throttle-20261019`, not `main`.

## Local test results
The complete xmake regression command on revision `28a734c4b18216c13322ac1e248daa525fc4af61` exited `0` in Termux/Android ARM64.
- `menu_config_value_test`: 64 checks, 0 failures.
- `aimassist_math_test`: 27 checks, 0 failures.
- `atomic_config_writer_test`: 10 checks, 0 failures.
- `diagnostics_test`: exit code 0.
- `autohit_guards_test`: 48 checks, 0 failures.
- `autohit_weapon_profile_test`: exit code 0.
- `autototem_policy_test`: 14 checks, 0 failures.
- Compiler emitted `g++: warning: argument unused during compilation: '-c'`; no test failed. This is a local Termux run, not the official GitHub Linux host test environment.

## Build/CI status
- Build #901: https://github.com/wendellestumano/Bedaim/actions/runs/39283185649 — failed at Android ARM64 build due to the three-vs-four argument compile error; host tests and NDK setup succeeded.
- Build #900: https://github.com/wendellestumano/Bedaim/actions/runs/39280609943 — earlier failure, not validation for this fix.
- No Actions run or check was found for PR head `28a734c4b18216c13322ac1e248daa525fc4af61` (Actions API returned zero runs/check-runs).
- `.github/workflows/build.yml` has a push branch allowlist excluding `fix/diagnostics-log-arity-20261019`. The `pull_request` trigger exists, but no run appeared. No `gh` CLI or Android NDK was found in Termux, so manual dispatch and a local Android package build were unavailable.
- A remote branch `ci/validate-diagnostics-log-arity-20261019` was created from the tested revision, but no workflow change or CI run was produced. Do not treat it as successful validation.

## Next steps
1. Obtain an actual GitHub Actions run for the fix branch using an authorized workflow-dispatch/approval path.
2. Inspect the Android ARM64 build log and verify that a `.levipack` artifact exists.
3. After CI passes, fully restart Minecraft/Levi and record FPS, AimAssist camera behavior, AutoHit route diagnostics, and actual damage separately. Native dispatch acceptance alone is not proof of server-side damage.
4. Keep PR #41 unmerged until CI and runtime evidence are available. Do not change `main`.
