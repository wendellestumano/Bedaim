# Bedaim progress — Build #898 baseline, AutoHit compile fix and FPS investigation

Date: 2026-10-20
Experimental branch: `experiment/build898-autohit-compile-fps-20261020`
Base branch: `perf/autohit-tick-throttle-20261019`
Baseline requested by user: Build #898. User reports #898 packages successfully but AutoHit remains nonfunctional; Builds #900 and #901 failed. In-game reports: Macefall/Shiftblade cause sudden lag spikes; FPS drops from 30–40 to 5–19 with Bedaim installed. Preserve BedrockAim and BedrockBound.

## Pre-edit evidence
- Source: `src/core/Diagnostics.hpp` declares only `log(Level, component, message)`.
- Source: `src/modules/player/autohit.cpp` still has two four-argument calls at the start/result diagnostics (`attack_attempt_begin` and `attack_attempt_result`). These are a definite source/API mismatch and align with the known diagnostics-arity build failure pattern.
- Source: `src/core/GameHooks.hpp` exposes two explicit `attack(...)` overloads and comments that the public route and guarded extended route have different ABIs. Do not alter attack ABI in this compile-only fix.
- Source: `src/modules/player/maceswapper.cpp` executes from `VerifiedLocalPlayerTickEvent`, acquires `m_stateMutex` at function entry, and holds it across player/inventory/target/GameMode operations. This is a plausible source of tick stalls, but no runtime profile yet proves it is the cause. Do not change it in the same patch.
- The documented `docs/PROGRESS_AUTOMATION_2026-10-19.md` path does not exist on the specified base branch; this dedicated progress file is being created instead.
- Branch source confirms the current AutoHit pipeline returns early when the native route is unavailable, with a five-second retry delay. Earlier user logs summarized in prior progress indicate `attack_route=unavailable`, `public_sig=0x0`, `survival_sig=0x0`; runtime attack remains unproven.

## Hypothesis recorded before source edits
The two AutoHit diagnostic calls pass event label and context as separate parameters even though the diagnostics API accepts only three parameters. Joining the event label and context into one message should remove this compile error without changing target selection, cadence, native route selection, or module behavior.

## Intended tests
1. Change only the two mismatched diagnostic calls.
2. Audit every `diagnostics::log` call in `autohit.cpp` against the declared signature.
3. Run host regression tests from `.github/workflows/build.yml`: menu config, AimAssist math, atomic config writer, diagnostics, AutoHit guards, AutoHit weapon profile, and AutoTotem policy.
4. Run the Android ARM64 build/package workflow; inspect actual job logs and require `BedrockTools.levipack` artifact generation before claiming build success.
5. Keep performance diagnosis separate: use runtime diagnostic logs/profiling to correlate spikes with Macefall/Shiftblade callbacks. Preserve BedrockAim and BedrockBound.
6. AutoHit runtime success requires attack dispatch evidence and server-confirmed damage in a test where the user does not manually attack. A successful compile or native wrapper return is not proof of damage.

## Initial status
- No source files changed at the time this hypothesis/test record was written.
- `main` is untouched. No merge is planned.
- Build #898 is the user's stated last tested baseline; Builds #900/#901 are reported failures, not assumed to have the same root cause until their logs are inspected.


## New evidence discovered after the first source correction (before next edit)
- Build run https://github.com/wendellestumano/Bedaim/actions/runs/39283185649 failed in Android ARM64 compilation at `src/core/GameHooks.cpp:2080`: the call to `diagnostics::log` passes four arguments, while the declaration accepts three. Its host regression tests passed; no artifact was produced.
- Build run https://github.com/wendellestumano/Bedaim/actions/runs/39297748350 failed at `src/modules/player/autohit.cpp:433` and `:469` with the same API mismatch. Its host regression tests passed; no artifact was produced.
- Updated hypothesis: the compile failure is not isolated to AutoHit; at least one analogous four-argument call in GameHooks remains in this branch. The AutoHit-only patch cannot yield an Android package until this independently evidenced mismatch is also fixed.
- Intended next test: combine the `native_attack_dispatch_result` event name with its existing `dispatchResult` string in one message argument, preserving all attack-route logic. Re-audit this call against the three-argument API, then rerun CI and inspect the actual build log/artifact. Do not modify attack ABI or FPS-related modules in this compile fix.
