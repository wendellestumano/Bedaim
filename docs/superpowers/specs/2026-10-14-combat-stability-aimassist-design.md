# Bedaim Combat and Aim Assist Stabilization — Design Spec

**Date:** 2026-10-14  
**Repository:** `wendellestumano/Bedaim`  
**Target branch:** `work/autohit-maceswapper-first-build-20261014`  
**Status:** Draft for review; no implementation authorization is implied by this document.

## 1. Goal

Make the existing Bedaim combat/aim path easier to validate and safer to test on Minecraft Bedrock 26.52 / LeviLauncher 1.5.25, prioritizing the reported crashes near entities, AimAssist orbiting or moving vertically past targets, target switching when multiple entities are nearby, and configuration values reverting after the menu is closed.

The work must remain isolated from `main`. No native attack route is considered runtime-safe merely because it compiles.

## 2. Current evidence and constraints

- The host regression workflow failed before compiling its test target. The log shows Xmake prompting for `nlohmann_json v3.11.3` and then exiting with “must be installed”; the Android NDK and package steps were skipped.
- `tests/launcher/menu_config_value_test.cpp` already covers boolean, integer, floating-point, string and unknown-key conversion, but the suite has not been observed passing in CI.
- `ModuleMenu.cpp` currently serializes the live module state, calls `loadConfig`, refreshes the AimAssist schema and flushes configuration after menu callbacks. This is a candidate flow to verify, not proof that every Mod Menu V2 callback maps to the correct key/value.
- AimAssist includes target filtering, virtual steering bounds, aim prediction, target hysteresis, and rotation smoothing. Reports of orbiting and vertical overshoot mean the next changes must begin with measurement and deterministic tests rather than tuning constants blindly.
- AutoHit dispatches through a native GameMode attack route that depends on the exact Bedrock ABI and a valid live GameMode. Desktop compilation cannot validate those native assumptions.
- The actual server-authoritative entity collision bounds are not modified by the existing virtual steering/Hitbox Expander bounds. UI text must continue to describe them as virtual.

## 3. Scope of the first implementation milestone

### A. Restore a trustworthy validation loop

1. Fix the host-test workflow so dependency setup is deterministic and non-interactive.
2. Run the menu conversion test target and the Android arm64 release package build in the same PR workflow.
3. Preserve the actual command output and failing step; a skipped build is never reported as a passing build.
4. Do not weaken or remove tests merely to turn CI green.

### B. Verify configuration persistence

1. A changed menu value is applied to the owning module, normalized/clamped by that module, serialized from the resulting live state, and flushed to disk.
2. Toggle, slider, text and choice controls use the exact key and payload contract supplied by Mod Menu V2.
3. Choice controls must tolerate both documented labels and legacy numeric indices without silently resetting unrelated options.
4. Invalid numeric/boolean payloads leave the previous setting untouched.
5. Reopening the menu or restarting the module must not unexpectedly turn AutoHit off or restore stale AimAssist values.

### C. Stabilize AimAssist target selection and rotation

1. Keep player/mob, range, field-of-view, visibility, whitelist, and selected-weapon filters explicit and independently testable.
2. Select one target deterministically per update. When two targets score similarly, use existing retention/hysteresis rules; do not flip targets on incidental iteration order.
3. Calculate angular error using wrapped yaw differences and finite, bounded values. Reject invalid positions/bounds instead of issuing camera deltas.
4. Keep predicted positions bounded in time and space, and do not feed a predicted point outside a validated target region into the rotation step.
5. Separate target selection, aim-point generation, and rotation-step calculation enough that each can be tested without calling Minecraft native functions.
6. Remove orbiting/overshoot only after tests or diagnostics identify which step is wrong. Avoid arbitrary speed multipliers as a substitute for diagnosis.

### D. Make AutoHit fail closed while ABI/runtime behavior is unknown

1. Do not call native attack dispatch when the local player, live GameMode, target, target bounds, hit point, range, or required trampoline/signature validation is unavailable.
2. Enforce a per-target attack interval and prevent recursive/synthetic attack re-entry.
3. Never infer that an attack succeeded just because an attempt was made; log attempt and native result separately.
4. Keep diagnostic logs rate-limited and avoid dereferencing unvalidated native pointers.
5. Do not claim that native dispatch is runtime-safe until manually tested on the target game build without crashes. Until then, expose a conservative fail-closed path rather than increasing attack aggressiveness.

## 4. Out of scope for the first milestone

These are follow-up milestones, not reasons to bundle unrelated changes into this stabilization patch:

- Auto Totem and inventory guard.
- Sword Swapper / Mace Swapper timing and priority behavior.
- ULTRAKILL HUD art refinements and hit-stop visuals.
- A server-authoritative Hitbox Expander; client-only steering bounds cannot provide that capability.
- Additional modes or new AimAssist features before existing target selection, persistence, and native calls are stable.

Each follow-up milestone must be specified and validated separately after the foundational regression suite is working.

## 5. Architecture and data flow

- **Menu callback layer:** parse the payload strictly, update the owning module, normalize through the module's configuration logic, serialize the live state, refresh its schema if needed, then persist.
- **Target selection:** consume a validated snapshot of local-player state and candidate actors; produce either one stable target plus a score or “no target”. It must not rotate the camera or attack.
- **Aim-point/prediction stage:** convert the selected target's validated native bounds and bounded motion estimate into one finite point inside the permitted steering region.
- **Rotation stage:** compute wrapped yaw/pitch error and a frame-time-aware bounded delta; reject non-finite inputs and avoid unbounded catch-up steps.
- **AutoHit stage:** consume the selected target and its validated native attack prerequisites, apply distance/cooldown/recursion guards, and report attempt/result separately. It must not change the AimAssist selection.
- **Native boundary:** all ABI-specific calls remain isolated and guarded; host tests must not execute native Minecraft calls.

## 6. Required tests and acceptance criteria

### CI / configuration
- The host workflow installs or resolves its dependencies without an interactive prompt.
- Boolean/numeric parsing tests pass, including malformed values, overflows, NaN/infinity and trailing garbage.
- A test verifies that the menu conversion path does not mutate unrelated keys.
- Configuration round-trip tests demonstrate that representative AimAssist and AutoHit settings retain normalized values across save/load.
- The Android arm64 build and `.levipack` packaging complete successfully before the change is presented as build-ready.

### AimAssist
- Given two candidates with near-equal scores, the current target remains selected unless the challenger crosses the configured switch margin.
- Yaw errors across the -180°/180° boundary take the short turn direction.
- Zero-length vectors, non-finite coordinates, invalid AABBs and missing native functions produce no camera delta.
- Prediction disabled yields the unpredicted target point; enabling prediction cannot create NaN/infinity or points outside validated bounds.
- The rotation step is bounded under low/high frame time and large target direction changes.
- Creative/spectator filtering, where the API can reliably identify those states, is covered by explicit tests; unknown state must not be guessed.

### AutoHit / runtime
- Missing target/GameMode/signature or invalid hit position skips attack dispatch.
- Cooldown prevents multiple dispatches inside the configured interval, and recursion guard prevents nested synthetic attacks.
- Attempt/result diagnostics distinguish skipped, attempted, cancelled and successful calls.
- Manual test on Bedrock 26.52 / LeviLauncher records whether AutoHit activates, whether normal manual attacks still work, and whether approaching entities causes any crash. Without that device test, runtime safety remains unverified.

## 7. Rollout and integration

1. Review and approve this specification.
2. Write a task-by-task implementation plan with tests first.
3. Fix the host validation workflow and exercise tests before modifying combat behavior.
4. Implement configuration round-trip coverage and correct any confirmed persistence defect.
5. Refactor/test the AimAssist math and target-selection seams without changing native signatures speculatively.
6. Harden AutoHit's preconditions and runtime diagnostics only after tests pin down its state/ABI assumptions.
7. Run host tests, Android build/package, and code review; retain all changes on the working branch.
8. Keep the PR unmerged until automated checks are green and runtime limitations are clearly documented.

## 8. Open risks

- The target Minecraft update may change private layouts, vtable slots, and native method signatures. No code-only host test can certify them.
- A successful Android build does not prove runtime stability or successful server-side hits.
- Some player game-mode states may not be exposed by a stable public API in this codebase; when unavailable, the filter must fail conservatively rather than read speculative offsets.
