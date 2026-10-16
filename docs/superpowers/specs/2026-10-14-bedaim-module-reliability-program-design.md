# Bedaim Module Reliability Program — Architecture Design

**Date:** 2026-10-14  
**Repository:** wendellestumano/Bedaim  
**Design status:** Submitted for maintainer review; implementation is not authorized until this document and the first-stage spec are approved.

## 1. Purpose

Make the Bedaim combat, inventory, HUD, and targeting modules dependable on the project's supported Minecraft Bedrock/LeviLauncher runtime. The goal is not merely to produce a successful native build: settings must persist, native calls must be ABI-safe, state machines must not race, and in-game behavior must be observable through reproducible tests.

The program covers the existing AutoHit and Aim Assist path, Mace/Sword Swappers, Auto Totem and shared inventory access, Hitbox Expander feasibility, and the ULTRAKILL HUD/hit-stop. Work will be executed in isolated, reviewable stages. No stage may be marked complete from CI compile success alone.

## 2. Constraints and non-goals

- Keep main untouched. All work goes to work/autohit-maceswapper-first-build-20261014 and the existing draft PR unless the maintainer explicitly chooses another branch.
- Target the Bedaim C++20 native module for Android ARM64/LeviLauncher. Native signatures and offsets are runtime-version-specific; verify against the target Minecraft build before relying on them.
- Preserve the current MIT license unless the repository maintainer explicitly decides otherwise.
- Do not execute the supplied Apollon APK. Its static contents can be examined only after the analysis tooling can read the binary reliably. Do not extract session tokens, credentials, private keys, or unrelated user data.
- Use public client projects as architectural/behavioral references, not as proof that their implementation is correct. Do not transplant GPL-3.0 code into this MIT-licensed repository without resolving licensing and attribution first.
- Do not claim that a client-side virtual AABB changes server-authoritative collision. That claim requires evidence from a controlled test environment.
- Do not make unrelated framework refactors or add dependencies without demonstrating that they are needed.

## 3. Current implementation facts (from the reviewed branch)

- The project is C++20 and builds with xmake; GitHub Actions builds the Android ARM64 target and packages a .levipack.
- src/config/ConfigManager.cpp serializes module settings, queues an asynchronous write, writes through a temporary file, and exposes flush(). A user-reported setting reset therefore needs a round-trip diagnosis covering UI callback, module serialization, normalization, file write, and reload—not a blind extra-save patch.
- src/launcher/ModuleMenu.cpp handles Mod Menu V2 callbacks. The Aim Assist schema has separate Aim, Targeting, Hitbox, Auto Hit, Weapons, and Special categories.
- AimAssistModule owns candidate selection, rotation, target snapshots, prediction settings, and m_autoHit state. The native GameHooks layer detours GameMode actions and publishes attack/result and frame events.
- Mace Swapper and Sword Swapper coordinate through shared swapper state; InventoryAccess reads item identifiers and inventory/offhand data via version-sensitive offsets/signatures.
- The repository has Python tests for tools/aim_trace_analyzer.py, but no established comprehensive C++ unit-test harness for native game objects.
- The branch's latest PR notes state that a native build succeeded but AutoHit ABI and EGL hit-stop still require actual in-game verification.

These observations describe the checked source, not proof that the runtime path is currently correct.

## 4. Architecture decisions

### 4.1 Separate decision-making from side effects

Target selection and hitbox/aim-point math must not invoke attacks. AutoHit must be a distinct action stage that consumes a validated target snapshot and uses the configured client attack-input mapping. Inventory modules must request changes through a shared coordinator rather than each issuing independent hotbar/offhand changes in the same tick. HUD and hit-stop react to published events and never initiate attacks or inventory changes.

### 4.2 Native boundary and runtime guards

At every native boundary:
- Validate the pointer and required functions before dereferencing/calling.
- Check all coordinates, rotations, AABB extents, ranges, and timing values for finiteness and valid limits.
- Do not retain a target pointer past the validated snapshot lifetime.
- Verify function signature/ABI against the supported runtime; do not guess a parameter list from a symbol name.
- Have explicit cooldown and at-most-once-per-tick/attempt rules; repeated calls must be bounded.
- On module disable, world/session change, target loss, or invalid state, clear owned transient state and release any render hold.
- Log state transitions and first failures at low frequency; do not log each frame or each failed candidate.

### 4.3 Configuration contract

The source module is the authority for its normalized setting values. The menu schema displays the normalized value after a change. A config update must round-trip as:

input control → parsed typed value → module normalization → module saveConfig → ConfigManager queued write/flush → file → ConfigManager load → same normalized module value → menu schema

Unknown keys and malformed values must not corrupt valid settings. Enum/radio values must support the format actually emitted by the current menu library, with explicit backward compatibility for saved values. Any regression test must fail if a setting turns off or resets after closing/reopening the menu or restarting the client.

### 4.4 Inventory mutation contract

Introduce a small shared inventory-action coordinator only after inspecting all relevant swapper and inventory access flows. It owns one pending inventory action at a time, the expected source/target slots, previous selected slot, timeout, completion/cancel state, and safe restoration. Modules declare intent/priority; the coordinator validates current inventory before changing it. Restoration must happen only if the slot still contains the item/state the module placed there, so it cannot overwrite a manual selection made by the player.

Auto Totem uses a configurable delay (default 100 ms, range 0–1000 ms) after observing that the offhand totem is consumed or missing. It must prefer the configured replace/keep behavior, not assume a totem exists, and cancel cleanly during container screens, unavailable inventory data, death/session transitions, or module disable.

### 4.5 AutoHit input source

Do not equate a control label with a numeric button ID. Inspect the actual LeviLauncher/client input API and trace its configured attack action. AutoHit should activate only when its conditions and the correct configured attack binding are satisfied; it must not synthesize a separate attack when the client input already submits one for the same action. The attack path must be instrumented to prove whether input was observed, a target was accepted, a dispatch occurred, and a native result returned.

### 4.6 Hitbox semantics

Track three different behaviors separately:
1. **Visualization:** draw/display altered bounds.
2. **Client targeting volume:** use a broader virtual region to select or steer toward an entity.
3. **Authoritative collision:** demonstrate that the authority performing collision/attack validation accepts a hit that the unmodified hitbox rejects.

Only items 1 and 2 can be implemented as local behavior without further evidence. The Hitbox Expander investigation must use an isolated world/server with a baseline and altered condition. If evidence supports only client-side effects, expose the module as a virtual targeting/visual tool and do not promise extra server reach.

### 4.7 Reference-source rules

- **WClient:** its public repo exposes modules named AutoTotemModule, HitboxModule, TriggerBotModule, HotbarSwitcherModule, plus module/value/config abstractions. It is packet-interception oriented; Bedaim is a native memory/event-hook project. Use it to identify edge cases, state transitions, and test scenarios, not to copy its packet dispatch into Bedaim.
- **Lumina Client:** useful for studying separated protocol/network/native layers and runtime compatibility boundaries. Its architecture differs from Bedaim's LeviLauncher module ABI.
- **Apollon APK:** the supplied Apollon_BlackDot_sign.apk is approximately 401,490,888 bytes. Current file tools confirm the upload but return no readable binary content, and local shell/Python analysis failed in this session. Therefore no claim is made yet about its manifest, symbols, native libraries, or implementation. Resume with offline static inspection only once a functioning binary-analysis environment is available.
- Bedaim's repository license is MIT; the checked Lumina and WClient repositories declare GPL-3.0. If code reuse is later proposed, stop and resolve compatibility/attribution before introducing it.

## 5. Staged roadmap

Each stage gets its own reviewed spec and implementation plan before code changes.

### Stage 0/1 — Diagnose configuration and AutoHit
Capture an exact baseline, write regression diagnostics, verify the config round trip and input mapping, and then correct only the confirmed failure. Prove the AutoHit path does not crash, duplicate an input attack, or attack from a stale/invalid target snapshot.

### Stage 2 — Inventory coordinator, Sword/Mace Swappers, Auto Totem
Make inventory operations mutually exclusive and safely reversible; test manual player inputs racing with module actions. Implement Auto Totem only after the coordinator contract is proven.

### Stage 3 — Aim Assist reliability
Address target selection/retention, yaw/pitch conventions, prediction, aim profiles, weapon profiles, and creative/spectator/team/name filters where the runtime exposes authoritative state. Add no-orbit and multi-target fixtures to the trace analyzer.

### Stage 4 — Hitbox Expander feasibility
Run controlled before/after tests that distinguish the visual box, local targeting behavior, actual attack submission, and authority-side hit acceptance. Gate any "real hitbox" claim on reproducible evidence.

### Stage 5 — ULTRAKILL HUD and hit-stop
Keep HUD display, attack-result events, and frame-hold behavior separated. Guarantee that hit-stop is bounded, disabled safely, does not deadlock or permanently suppress swaps, and remains performant on Android.

### Stage 6 — Integration and release validation
Run available tests and Android ARM64 CI, inspect artifacts, install the produced package on the target environment, and run the checklist below. Record exact branch, commit, Minecraft build, device/runtime, test results, and known limitations.

## 6. Acceptance matrix

| Area | Required evidence |
|---|---|
| Config | Each test setting survives menu close/reopen and client restart; normalized values are consistent on disk and in UI. |
| AutoHit | Configured attack binding is observed; exactly one bounded dispatch occurs when valid; no dispatch for invalid/stale target; no crash or duplicate attack. |
| Targeting | Target changes are deterministic; two targets do not cause alternating oscillation; non-finite bounds/rotations are rejected. |
| Inventory | Swap operations do not overlap; original slot is restored only when safe; manual slot changes are preserved. |
| Auto Totem | Finds a real totem, respects delay/replace settings, responds to consumption once, and safely no-ops when no totem/data is available. |
| Hitbox | Visual and client-virtual effects are separately measured; authority-side collision is claimed only if independently verified. |
| HUD/hit-stop | Positive attack result reaches HUD; hold is bounded and released on disable/session loss; no persistent freeze or high-frequency log spam. |
| Delivery | CI success, artifact validity, in-game checklist, commit hash, build/runtime version, and explicit limitation list are all recorded. |

## 7. Initial diagnostic instrumentation

Diagnostics should report only the minimum state needed to classify a failure: timestamp, Minecraft build, branch/commit, enabled module state, config key/value before and after normalization, current attack binding identifier (not account data), target identity token local to that process, snapshot age, range/LOS acceptance, whether dispatch was attempted, native result, and inventory action state. Avoid player names and credentials in persistent logs. Logs must be rate-limited and disable-able.

## 8. Decisions that must remain explicit

- The exact attack-button representation is not yet verified.
- It is not yet proven whether AutoHit's current native function dispatch matches the actual client's normal attack path.
- It is not yet proven whether a Hitbox Expander can affect server-authoritative collision in the target setup.
- Auto Totem and any inventory coordinator must be validated against Bedrock 26.52 before release claims.
- Static analysis of the supplied Apollon APK remains pending because binary inspection failed in this session.

## 9. Reference URLs

- Bedaim: https://github.com/wendellestumano/Bedaim
- WClient: https://github.com/RetrivedMods/WClient
- Lumina Client: https://github.com/TheProjectLumina/LuminaClient
