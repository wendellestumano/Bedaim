# AutoHit and Configuration Reliability — Implementation Plan

> **For agentic workers:** Execute this plan task-by-task. Steps use checkbox syntax; tests must be written and observed failing before the implementation step.

**Goal:** Diagnose and fix the reported Aim Assist setting resets and AutoHit reliability/crash risks without changing the meaning of the configured attack action or claiming in-game success from compilation alone.

**Architecture:** Extract menu-value conversion into a small, host-testable component. Add explicit persistence outcomes and low-frequency diagnostics, then trace the actual LeviLauncher attack input against the native GameMode action path. Keep AutoHit decisions separate from native side effects and test the decision guards before changing dispatch behavior.

**Tech Stack:** C++20, nlohmann/json, xmake, Python standard library for optional diagnostic tools, GitHub Actions, Android/ARM64/LeviLauncher runtime.

**Spec:** docs/superpowers/specs/2026-10-14-bedaim-module-reliability-program-design.md

## Global Constraints

- Keep main untouched; work on work/autohit-maceswapper-first-build-20261014 and the existing draft PR.
- Target Android ARM64 / LeviLauncher and verify runtime-sensitive signatures against the intended Minecraft build.
- Do not guess the numeric representation of the client's configured attack action.
- No unbounded attack loops, duplicate dispatch for one input action, stale target pointer use, or persistent render holds.
- Do not use a client-side virtual AABB as proof of server-authoritative collision changes.
- Keep diagnostic logs low-frequency and exclude player names, tokens, credentials, and unrelated config values.
- A successful native build alone is not acceptance; report automated tests and in-game checks separately.

## Review Focus

1. **Malformed menu payload:** invalid boolean/number input must preserve the prior valid setting instead of mutating unrelated state. Owned by Task 1 parser tests.
2. **Radio/select representation:** values may arrive as labels or numeric indices; normalization must remain backward-compatible. Owned by Task 1 and Task 2 round-trip checks.
3. **Disk write failure:** flush must not imply durability if the asynchronous writer failed. Owned by Task 2 tests and diagnostics.
4. **Touch versus semantic attack action:** raw touch events do not prove that the client attack button was pressed. Owned by Task 3 runtime trace.
5. **Nested native action or duplicate call:** an AutoHit-generated action can re-enter the attack detour; one configured action must not generate an uncontrolled second attack. Owned by Task 4 dispatch-cardinality tests.

---

### Task 1: Make menu-value conversion independently testable

**Files:**
- Create: src/launcher/MenuConfigValue.hpp
- Create: src/launcher/MenuConfigValue.cpp
- Create: tests/xmake.lua
- Create: tests/launcher/menu_config_value_test.cpp
- Modify: src/launcher/ModuleMenu.cpp
- Modify: .github/workflows/build.yml

**Interfaces:**
- Produces: bedaim::menuconfig::ApplyResult applyValue(nlohmann::json& config, std::string_view key, std::string_view rawValue).
- ApplyResult has Applied and InvalidValue cases.
- The helper modifies only the requested key. Existing string values, missing keys, and null values accept raw string payloads so radio/select settings can be normalized by the module afterward.
- Existing boolean, integer, and floating-point fields are parsed strictly. Malformed values and non-finite floats return InvalidValue and leave the JSON unchanged.

- [ ] **Step 1: Write failing parser tests**
  - Test valid boolean spellings true/false, 1/0, on/off and yes/no.
  - Test malformed booleans leave the value unchanged.
  - Test signed integers and decimal floats parse without trailing garbage.
  - Test empty, overflow, NaN, Infinity and malformed numeric payloads do not change the old value.
  - Test string/radio values remain strings and unrelated JSON fields remain unchanged.

- [ ] **Step 2: Run tests and confirm failure**
  - Run from repository root:
    cd tests && xmake f -p linux -a x86_64 -m debug && xmake && xmake run menu_config_value_test
  - Expected: the test target initially fails to configure or compile because the helper and/or test target does not exist.

- [ ] **Step 3: Implement the small parser and wire it into ModuleMenu**
  - Move only the conversion logic currently embedded in onModuleConfigChanged into the helper.
  - Preserve module normalization via loadConfig and serialization via saveConfig.
  - Do not make the schema or disk file the authority for a module's live value.
  - Keep invalid payload handling a no-op for the requested field.

- [ ] **Step 4: Run parser tests**
  - Run the command from Step 2.
  - Expected: all parser cases pass, including non-mutation on malformed values.

- [ ] **Step 5: Add the host test to CI**
  - Add a Linux host-test step to the existing workflow before the Android ARM64 build step.
  - Initially run the menu parser target; extend the same step in Tasks 2 and 4 to run config-writer and AutoHit-policy targets when they exist.
  - Run the Android build/package job unchanged afterward. Expected: CI reports host tests and Android package build as separate steps.

- [ ] **Step 6: Commit the isolated parser**
  - Commit message: test: isolate and verify menu config value parsing

### Task 2: Make configuration persistence observable and test the round trip

**Files:**
- Create: src/config/ConfigFileWriter.hpp
- Create: src/config/ConfigFileWriter.cpp
- Modify: src/config/ConfigManager.hpp
- Modify: src/config/ConfigManager.cpp
- Modify: src/launcher/ModuleMenu.cpp
- Create: tests/config/config_persistence_test.cpp
- Modify: tests/xmake.lua
- Modify: .github/workflows/build.yml

**Interfaces:**
- Provide bedrocktools::config::writeAtomically(const std::string& path, const std::string& payload) -> bool in ConfigFileWriter.hpp/.cpp. It owns the temporary-file/rename operation and is testable without ModuleRegistry or Minecraft objects.
- Change ConfigManager::flush from void to bool. It returns true only when the latest effective snapshot queued before the call has been written successfully; on a write error it returns false. Audit all existing call sites after changing the return type.
- Menu callbacks use the returned result to log a concise failure. Log the module id, key, JSON type, parse result and write outcome only; never log raw values or the full config.

- [ ] **Step 1: Write failing persistence tests**
  - Use a temporary directory and temporary JSON path, never the live Android config path.
  - Test the new writeAtomically function directly for successful write, valid JSON bytes, overwrite/rename behavior, and the false result for an unusable destination.
  - Add ConfigManager-level integration coverage only if it can run with a lightweight test seam; do not link or mock the complete Minecraft module registry just to test file writing.

- [ ] **Step 2: Run the persistence tests and confirm failure**
  - Run: cd tests && xmake f -p linux -a x86_64 -m debug && xmake && xmake run config_persistence_test
  - Expected: the initial test fails to compile or assert because ConfigFileWriter does not yet exist.

- [ ] **Step 3: Implement truthful write status**
  - Implement writeAtomically using the existing temporary-file/rename strategy and explicit error reporting.
  - Propagate its result through the worker to the completed generation observed by flush.
  - Preserve asynchronous debounce and wait-for-completion behavior; ensure shutdown paths safely ignore or record the result without throwing.

- [ ] **Step 4: Verify save/flush/reload**
  - Test a temporary JSON round trip using the writer and the parser from Task 1 with typed toggle, integer, float, and radio/select values.
  - Confirm failed writes are observable and a successful write reloads to the same JSON value.
  - Treat module normalization and the full Mod Menu close/reopen/restart path as explicit in-game integration checks in Task 5, not as already-covered unit tests.

- [ ] **Step 5: Add minimal runtime diagnostics**
  - In ModuleMenu, log only the module id, setting key, JSON type, accepted/rejected parse status and whether the write completed.
  - Never log raw values, whitelist text, the entire JSON document, or account data.
  - Ensure diagnostics are emitted on menu change, not every player tick or render frame. Do not report success when flush returns false.

- [ ] **Step 6: Run tests and commit**
  - Run both host test targets and the Android build.
  - Commit message: fix: report config persistence failures

### Task 3: Trace the actual configured attack action before changing AutoHit behavior

**Files:**
- Inspect: LeviLauncher input API and the current GameHooks callbacks.
- Modify only if needed after the trace confirms an instrumentation gap: src/core/GameHooks.cpp and src/modules/player/aimassist.cpp.
- Add a short test procedure to docs/superpowers/plans/2026-10-14-autohit-config-reliability.md or a focused diagnostic README.

**Interfaces:**
- LeviLauncher's documented pl::input events expose raw touch (action, pointerId, x, y), key (keyCode, unicodeChar, isKeyDown) and mouse (button, isDown) events. These are not, by themselves, a named semantic attack-button event.
- Existing GameHooks detours expose native GameMode action/attack events and results. Treat the correspondence between a raw input and a native action as unknown until demonstrated on the target runtime.

- [ ] **Step 1: Record the source-based hypothesis**
  - Write down the configured input binding or touch action reported by the client and the native GameMode callbacks currently reached.
  - Do not assume button1, mouse-left, a specific touch coordinate, or a key code is the attack action.

- [ ] **Step 2: Add or adapt a diagnostic-only trace if necessary**
  - Record monotonic timestamp, event class, action/state, and a process-local sequence id.
  - Use rate-limited logs and do not consume or block inputs.
  - Register listeners once because the published API has no unregister callback; route behavior through a disabled-by-default diagnostic flag.

- [ ] **Step 3: Build and install the diagnostic version**
  - Run the host tests and Android ARM64 CI build.
  - Confirm the produced package exists and includes the expected branch commit.

- [ ] **Step 4: Run the isolated in-game observation**
  - In a private single-player/test world, exercise the configured attack action with no target, a nearby target, an out-of-range target, and a target that disappears.
  - Capture the raw input trace and native GameMode action/result trace for each case.
  - Expected: the actual path from configured input to native action is identified; if evidence is ambiguous, stop and request more runtime data rather than guessing.

- [ ] **Step 5: Document the mapping and commit**
  - Record the specific Minecraft build, LeviLauncher version, input mapping, trace summary, and uncertainty.
  - Commit message: test: trace configured attack input path

### Task 4: Gate AutoHit decisions and prove dispatch cardinality

**Files:**
- Create: src/modules/player/AutoHitDispatchPolicy.hpp
- Create: tests/player/auto_hit_dispatch_policy_test.cpp
- Modify: tests/xmake.lua
- Modify only after the tests pass: src/modules/player/aimassist.cpp and, if the trace requires it, src/core/GameHooks.cpp

**Interfaces:**
- The policy helper is a pure C++ function that accepts an immutable snapshot of prevalidated booleans/values: module enabled, configured attack action observed, local player valid, target valid, target snapshot fresh, bounds valid, target in range, line of sight accepted, cooldown ready, and action already dispatched.
- It returns whether a dispatch attempt is permitted. It must not dereference game pointers or call a native function.

- [ ] **Step 1: Write failing policy tests**
  - One valid input/action permits one dispatch.
  - Disabled module, absent configured input action, missing player/target, expired snapshot, invalid bounds, out-of-range target, blocked line of sight, or active cooldown each deny dispatch.
  - A second evaluation for the same action sequence is denied once the action is marked dispatched.
  - NaN/infinite range or age is rejected.

- [ ] **Step 2: Run policy tests and confirm failure**
  - Run: cd tests && xmake f -p linux -a x86_64 -m debug && xmake && xmake run auto_hit_dispatch_policy_test
  - Expected: the test initially fails because the policy helper is not implemented.

- [ ] **Step 3: Implement the pure policy helper**
  - Keep all native calls out of the helper.
  - Make the action sequence token and per-action once-only rule explicit at the call site.
  - Preserve target-selection behavior unless the input trace proves a change is necessary.

- [ ] **Step 4: Integrate the gate after input mapping is verified**
  - Do not introduce guessed control IDs.
  - Do not dispatch an additional synthetic attack if the original configured action already made the native attack call.
  - Retain pointer, range, finite-coordinate and session-state checks immediately before the native call.
  - If the intended input cannot be reliably observed, leave AutoHit disabled by default and document the blocker instead of shipping speculative dispatch code.

- [ ] **Step 5: Run tests and inspect logs**
  - Run parser, persistence and policy tests.
  - Verify logs distinguish input observed, policy accepted/rejected, dispatch attempt and native result.
  - Expected: at most one dispatch per configured action and zero dispatches for each invalid-state test.

- [ ] **Step 6: Commit the policy and integration**
  - Commit message: fix: gate AutoHit dispatch on validated attack input

### Task 5: End-to-end validation and handoff

**Files:**
- Modify only if test results demand it: relevant tests, diagnostics and this plan's checklist.
- Update: the existing draft PR description with exact validation evidence; keep the PR in draft.

- [ ] **Step 1: Run all host tests**
  - Run: cd tests && xmake -P . f -y -p linux -a x86_64 -m debug && xmake -P . -y && xmake -P . run menu_config_value_test && xmake -P . run config_persistence_test && xmake -P . run auto_hit_dispatch_policy_test
  - Expected: all three test executables compile and run with zero failures.

- [ ] **Step 2: Run the Android ARM64 build**
  - Run the current repository workflow, using Android NDK r28c.
  - Expected: the native library and .levipack package are generated.

- [ ] **Step 3: Perform runtime acceptance checks**
  - Toggle AutoHit and representative settings; close/reopen the menu and restart the client.
  - Test the configured attack action in the isolated world at valid and invalid target states.
  - Test disable, target disappearance and session/world transition.
  - Expected: values persist; no crash, duplicate attack, stale-target attack, or stuck state.

- [ ] **Step 4: Report evidence without overclaiming**
  - Record branch and commit hashes, host test output, CI run, artifact name, Minecraft and LeviLauncher versions, and in-game outcomes.
  - If runtime validation cannot be performed, mark the native behavior unverified and keep the PR draft.

- [ ] **Step 5: Commit validation notes**
  - Commit message: docs: record AutoHit and config validation

## Execution notes

- Ruling: xmake host commands explicitly use `-P .` from `tests/` — the first CI attempt discovered the repository-root xmake project and tried to resolve Android-only `preloader` for Linux before compiling tests; cost if wrong: CI still selects the root project and requires another small command adjustment.
- Tasks 1 and 2 are related by configuration state but should retain independent test cycles.
- Task 3 must complete before Task 4 can connect AutoHit to an input source.
- Do not begin the later inventory, AimAssist overhaul, Hitbox Expander, or HUD/hit-stop stages until Stage 1 is reviewed and its acceptance criteria are met.
