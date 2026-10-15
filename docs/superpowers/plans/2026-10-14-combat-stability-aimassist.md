# Combat and Aim Assist Stabilization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore deterministic CI validation, verify durable Mod Menu configuration, then remove the highest-risk sources of AimAssist oscillation and AutoHit crashes.

**Architecture:** Keep menu payload parsing independent from module persistence. Extract only deterministic selection/rotation math that can run without a live Minecraft process, and retain native interaction behind the existing runtime guard. Build/test each layer before proceeding to the next layer.

**Tech Stack:** C++20, Xmake, nlohmann_json, GitHub Actions, Android NDK r28c, BedrockTools SDK.

**Spec:** `docs/superpowers/specs/2026-10-14-combat-stability-aimassist-design.md`

## Global Constraints

- Target runtime: Minecraft Bedrock 26.52 and LeviLauncher 1.5.25.
- All changes stay on `work/autohit-maceswapper-first-build-20261014`; do not merge to `main`.
- Native GameMode/Actor calls must not be invoked in host regression tests.
- The Hitbox Expander is client-side virtual steering geometry only; do not claim server-authoritative hitbox changes.
- A successful host test/build is not proof of in-game ABI safety; report the distinction.

## Review Focus

- CI package acquisition must not block on an interactive prompt; prove with the exact host test command in Actions.
- Invalid menu payloads must preserve the prior value and unrelated keys; prove with existing and expanded host tests.
- Menu callback state must be persisted from the normalized live module, not a discarded local JSON object; test round-trip semantics.
- Non-finite positions, inverted/degenerate bounds and yaw wraparound must not produce invalid rotation deltas; unit-test a native-free math seam.
- Missing or stale target/GameMode data must never trigger a native attack; isolate and test precondition/cooldown logic where possible.

---

### Task 1: Make host regression CI non-interactive

**Files:**
- Modify: `.github/workflows/build.yml`
- Test: `tests/xmake.lua`, `tests/launcher/menu_config_value_test.cpp`

**Interfaces:**
- Consumes the existing `menu_config_value_test` target and `nlohmann_json v3.11.3` Xmake requirement.
- Produces a workflow step that installs/resolves the exact host test dependency before compiling the tests and reports test/build failure without skipping silently.

- [ ] **Step 1: Reproduce and capture the failure condition**
  - Run the current workflow once (or inspect the latest failure) and verify whether it stops at “packages(nlohmann_json): must be installed”.
- [ ] **Step 2: Change dependency setup only**
  - Add an explicit non-interactive Xmake package-requirement installation for `nlohmann_json v3.11.3` before configuring/building the host test target. Keep existing test assertions and Android build steps intact.
- [ ] **Step 3: Verify the workflow**
  - Run host configuration/build/test and the Android arm64 release package build.
  - Expected: host test output ends with “checks, 0 failures”; Android job produces `build/android/arm64-v8a/release/BedrockTools.levipack`.
- [ ] **Step 4: Commit**
  - `git commit -m "ci: install host test dependencies non-interactively"`

### Task 2: Prove menu-value parsing and configuration round-trips

**Files:**
- Inspect/modify: `src/launcher/ModuleMenu.cpp`
- Inspect/modify: `src/config/ConfigManager.cpp` and its header
- Inspect/modify: `src/modules/Module.cpp` and `src/modules/ModuleRegistry.cpp` only if required by the actual data flow
- Modify: `tests/launcher/menu_config_value_test.cpp`
- Modify: `tests/xmake.lua`

**Interfaces:**
- Existing `bedaim::menuconfig::applyValue(nlohmann::json&, std::string_view, std::string_view) -> ApplyResult` remains the strict conversion seam.
- Persist module state using the repository's actual ConfigManager/registry API; do not add a parallel persistence mechanism.

- [ ] **Step 1: Trace current write/read path**
  - Identify where `ModuleMenu.cpp`'s local `live` JSON object should be merged into persistent configuration and whether `ConfigManager::save()` serializes its own snapshot or asks modules directly.
- [ ] **Step 2: Add a failing regression test**
  - Test that representative boolean, numeric and choice values survive the same serialization/deserialization normalization path used by the menu, and invalid values leave the old value untouched.
- [ ] **Step 3: Run the targeted host test**
  - Expected: the new round-trip assertion fails for the observed root cause before the production fix.
- [ ] **Step 4: Implement the smallest data-flow fix**
  - Update the persistent module object from the normalized live state using existing ConfigManager APIs; remove discarded/unneeded local snapshots only after behavior is covered.
- [ ] **Step 5: Run host regression suite and Android build**
  - Expected: all conversion and round-trip tests pass, and the Android arm64 package builds.
- [ ] **Step 6: Commit**
  - `git commit -m "fix: persist normalized module menu settings"`

### Task 3: Unit-testable AimAssist angle and point calculations

**Files:**
- Create: `src/modules/player/AimAssistMath.hpp` (or equivalent native-free utility)
- Create: `tests/launcher/aimassist_math_test.cpp`
- Modify: `tests/xmake.lua`
- Modify: `src/modules/player/aimassist.cpp` only after the test shows a required behavior change

**Interfaces:**
- Native-free functions must take scalar/vector values and return values; they must not read Actors, call vtables, access Bedrock memory or mutate camera state.
- Keep the smallest useful surface: finite/bounds validation for scalar AABB data, wrapped yaw error, and a bounded angular step. Use exact types that compile standalone with C++20.

- [ ] **Step 1: Add tests for expected behavior**
  - Yaw difference from +179° to -179° is +2° in the shortest direction.
  - Non-finite values are rejected.
  - Invalid/inverted bounds and non-positive dimensions are rejected.
  - Angular step is finite and cannot exceed the specified maximum turn rate times clamped frame delta.
- [ ] **Step 2: Build/run tests and observe RED**
  - Expected: tests fail because the native-free functions are not yet implemented.
- [ ] **Step 3: Implement only those functions**
  - Keep all degree wrap and finite checks centralized; clamp frame delta to the spec's existing safe range (0.002–0.05 seconds).
- [ ] **Step 4: Integrate the tested helpers**
  - Replace duplicate local math only where signatures and behavior match; preserve target-selection heuristics until dedicated tests prove them wrong.
- [ ] **Step 5: Run all host tests and Android build**
  - Expected: no invalid delta generated by the helper tests; full package build completes.
- [ ] **Step 6: Commit**
  - `git commit -m "test: cover aim assist angle and bounds math"`

### Task 4: Make AutoHit guard decisions independently testable

**Files:**
- Create: `src/modules/player/AutoHitGuards.hpp` (native-free helper if the traced code supports it)
- Create: `tests/launcher/autohit_guards_test.cpp`
- Modify: `tests/xmake.lua`
- Modify: `src/modules/player/aimassist.cpp` and/or `src/core/GameHooks.cpp` only after tracing confirms the faulty precondition path

**Interfaces:**
- Guard helper takes only observed booleans/timestamps/distances and returns a decision; it never calls native attack functions.
- Actual `gamehooks::attack` remains callable only after all preconditions pass.

- [ ] **Step 1: Add tests for missing/stale target, missing GameMode/signature, out-of-range hit point and active cooldown**
  - Expected for each invalid prerequisite: no dispatch.
- [ ] **Step 2: Run targeted tests and observe RED**
- [ ] **Step 3: Implement a native-free guard decision and integrate it**
  - Do not change native ABI signatures or vtable offsets during this step.
- [ ] **Step 4: Run all host tests and Android build**
- [ ] **Step 5: Commit**
  - `git commit -m "fix: fail closed on invalid AutoHit state"`

### Task 5: Final review and test package handoff

**Files:**
- Review the complete branch diff and `docs/superpowers/specs/2026-10-14-combat-stability-aimassist-design.md`.
- Update PR #1 description with actual verification results only.

- [ ] **Step 1: Re-run host regression tests and Android arm64 package build from the final head**
- [ ] **Step 2: Inspect the complete diff for accidental changes, unused snapshots, unsafe pointer dereferences and claims that exceed the tests**
- [ ] **Step 3: Request/review code review and keep the PR unmerged**
- [ ] **Step 4: Provide the user the exact artifact and short in-game checklist**
  - In-game checklist: load the package; confirm menu values survive closing/reopening; run AimAssist at one stationary target, one moving target and two nearby targets; only then toggle AutoHit and approach a target while watching for a crash. Report any crash before enabling AutoHit again.
