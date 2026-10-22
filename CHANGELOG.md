## 2026-10-22 — Follow Apollon NormalTick identity and preserve native ABI

- Compared the uploaded Apollon source directly: its `NormalTick(LocalPlayer *thiz)` wrapper has a `void*` return type and assigns `localPlayer = thiz` from the exact LocalPlayer tick hook. BedAim now uses a pointer-returning hook signature and returns the original game's value unchanged.
- BedAim now records the validated NormalTick receiver as the strong LocalPlayer source. The ClientInstance getter is skipped after this route succeeds, so a known-null getter cannot erase the fresher `this` pointer.
- A concurrent getter failure is also prevented from clearing a pointer that the NormalTick hook has captured. Both instance transitions and hook uninstall clear the provenance marker.
- The uploaded Apollon README identifies its codebase as Minecraft 1.21.50; no version-specific absolute offsets were copied. Only the normalTick identity/ABI pattern was used.
- Host and Android ARM64 CI are required; the latest build has not yet been tested in-game.

## 2026-10-21 — Validate GameHooks receiver and LocalPlayer getter identity

- Removed circular `ClientInstance::current()` calls from GameHooks recovery and callback validation. In this project, `sdk::ClientInstance::current()` calls `Api.cpp`, whose provider returns `GameHooks::clientInstance()`; that merely reads the same cache being diagnosed.
- The ClientInstance update receiver is now accepted only when its vtable contains the exact resolved `ClientInstance::update(bool)` entry (or its ARM64 branch veneer). A generic vtable with a few Minecraft methods is no longer enough.
- The LocalPlayer getter signature is accepted only when its implementation also appears uniquely in the ClientInstance vtable near the documented getter slot. A signature hit in an unrelated function is never called through this route.
- The documented slot remains a guarded fallback on the verified ClientInstance; both getter routes still require `Actor::isPlayer` before caching a result.
- Added explicit diagnostics for receiver-vtable identity and getter-signature matches/misses. New experimental revision; host/Android CI and in-game verification required. `main` remains untouched.

## 2026-10-21 — Recover AutoHit LocalPlayer from validated tick identity

## 2026-10-21 — Restore GameHooks tick delivery and validate ClientInstance identity

- Restored the legacy `LocalPlayerPreTickEvent` / `LocalPlayerTickEvent` stream for every hooked `NormalTick` actor, matching the Build #614 event contract. Exact-local-only work remains gated behind `VerifiedLocalPlayerPreTickEvent` and `VerifiedLocalPlayerTickEvent`.
- Added a throttled cross-check of `ClientInstance::current()` against the signature-pattern update-hook receiver and prevent the hook from overwriting a validated SDK instance with a different receiver.
- Added a low-volume runtime probe every 2048 tick calls so gameplay logs can distinguish “hook installed” from “hook actually executing.”
- Host/Android CI and in-game verification are required; this change does not claim the LocalPlayer getter is proven on-device.


- Added a fallback for the logged case where no exact ClientInstance::getLocalPlayer export exists and the documented vtable getter repeatedly returns null.
- The NormalTick hook now retries LocalPlayer recovery independently of the ClientInstance-update resolver, so the update hook cannot starve the fallback's retry timer.
- Candidate capture requires a live validated ClientInstance and exact ownership of the same Player pointer; a shared `NormalTick` vtable match alone is no longer treated as local-player identity. Actor::isPlayer is also required. No guessed vtable slot is called and no candidate pointer is inferred from arbitrary data.
- Candidate scans are throttled to 250 ms and diagnostics identify the successful capture route. The existing attack ABI correction is preserved; all other modules are unchanged.
- Experimental only. Build validation and a new in-game log must confirm captured_from_normal_tick_vtable or captured_from_clientinstance_player_field, followed by real AutoHit behavior.

## 2026-10-21 — Fix AutoHit hit-position ABI

- Corrected the guarded four-argument GameMode attack ABI: argument four is a pointer/reference to the target contact Vec3, not a boolean result-out pointer.
- Updated the native detour/trampoline signature and forwarded the hit position consistently through both the public adapter and validated internal route.
- Removed the invalid native bool-out success gate. A synthetic attack is accepted only when the validated internal observer actually runs and reports a true native return; this remains distinct from server-confirmed damage.
- AutoHit diagnostics now report the selected four-argument ABI and route acceptance rather than the nonexistent native output flag.
- Based on the documented LeviLamina GameMode::_attack(Actor&, bool, Vec3 const&) signature and the uploaded logs repeatedly showing native_out_result=false despite a true wrapper/internal return. Experimental branch only; ARM64 CI and in-game target-damage testing are still required.

## 2026-10-21 — AutoHit normal input-callback experiment

- Added an exact-export resolver for supported ClientInputCallbacks attack-button handlers (newer handleBuildOrAttackOrBlockSelectButtonPress and older destroy/attack variants). No hard-coded address or guessed vtable slot is used.
- The callback path is eligible only when the symbol resolves inside the active Minecraft image, the receiver equals the current ClientInstance, and its resolved local player matches AutoHit's expected player.
- AutoHit prefers this normal engine input path when all checks pass, retaining the existing guarded GameMode route only when the callback is unavailable.
- Callback invocation is logged separately from native-route acceptance; it does not prove that an attack or server damage occurred. Requires Android ARM64 build and in-game verification. This branch remains experimental and main is untouched.
- Cross-checked the public Apollon wrapper: it calls the legacy native handler with a ClientInstance receiver, but its hard-coded address was not reused because it is stale for the current binary. The resolver relies only on exact exported symbols.

## 2026-10-21 — AutoHit-only revival branch (experimental)

- Restored **AutoHit** to the module registry on a branch based on the latest no-AutoHit/no-AutoTotem release candidate.
- **AutoTotem remains unregistered**; no other module implementations or block-break mitigation changes are included in this branch.
- This is a controlled reactivation step, not a claim that attacks work. The experimental branch now attempts exact-symbol resolution for the normal input callback, but the target-build symbol/ABI and actual damage still require in-game validation.
- Keep this branch isolated. Require host regression tests, Android ARM64 packaging, and in-game confirmation of real target damage before treating AutoHit as restored.

## 2026-10-21 — Latest experimental build without AutoHit and AutoTotem

- Based on `experiment/actor-vector-alignment-guard-20261021-run5`, preserving the latest experimental AimAssist/vector validation and diagnostics changes.
- Removed AutoHit and AutoTotem from `ModuleRegistry`: their headers are no longer included and neither module is registered or exposed in the module menu.
- No implementation code for other modules was edited by this release-specific change. The build and regression tests must pass before treating the package as verified.

## 2026-10-21 — Actor snapshot alignment validation (experimental)

- Bind the AimAssist actor-vector validator's alignment requirement explicitly to `alignof(DistanceSortedActor)` instead of relying on the pointer-alignment default.
- Add host regression cases for a misaligned base address, a non-power-of-two alignment, and a stride that is incompatible with the requested alignment.
- Defensive metadata validation only; no crash or FPS improvement is claimed without CI and in-game evidence.

## 2026-10-20 — AutoHit target-filter and route-failure attribution (experimental)

- Added a throttled per-filter target-scan breakdown for the no-target state, including player/mob toggles, creative/spectator filtering, whitelist, invisibility, geometry, line of sight, range, FOV, and missing native-filter availability.
- Added a throttled reason for native route unavailability: null GameMode, GameMode not observed by the runtime hook, or no validated vtable entry. Object addresses are included to compare runtime identity checks.
- Diagnostic-only change. No attack ABI, offsets, target eligibility, or dispatch behavior was changed. Build and in-game logs are still required before selecting a fix.

## 2026-10-19 — AutoHit route ABI fix after runtime evidence (experimental)

- Replaced the layout-only fallback that accepted vtable slot 16 whenever `useItem` appeared at slot 12. Neighboring readable function pointers do not prove method identity or ABI.
- Added a separately validated four-argument route for guarded ARM64 thunks that have both a conditional guard/alternate return and a direct tail branch to the resolved `GameModeAttack` implementation. The live vtable entry is called using its validated four-argument ABI; validated public three-argument adapters remain preferred when present.
- The resolver scans for known public adapters even when exports are stripped, and emits the selected slot, ABI, and implementation target. If the native observer installed successfully but does not see the call, a public wrapper's `true` return is no longer reported as an accepted attack.
- Changed the immediate local `hurtTime` threshold from an attack-dispatch lockout to a one-shot diagnostic warning. Hurt-time is sampled before authoritative server state necessarily arrives; the configured weapon CPS remains the repeat limiter.
- Root-cause evidence: the user-provided runtime log dispatched through slot 16, reported `public_virtual_returned_true`, did not reach the internal observer, and recorded target hurt-time `0 -> 0`. The user's isolated Termux test against the matching `libminecraftpe.so` reports slot 2 branching to `0xF89FE18` and slot 16 branching to `0xF89FA80`; this supports removing slot-16-by-layout inference. No hard-coded slot is used by the new resolver.
- ProtoHax UserScript's `interactEntity(..., "attack")` is a protocol/session action, not a native C++ `GameMode` call, so it remains an architectural reference rather than a directly copied function. The Apollon APK config contains `emulateNativeAttack` and KillAura settings, but the stripped `libReverseSigma.so` strings alone do not establish its implementation ABI.
- Experimental branch only. Build and host regression tests are required; a successful build does not prove server-confirmed damage. In-game verification is still pending.

## 2026-10-19 — AutoHit native-route retry throttling (experimental)

- Increased the AutoHit retry delay after an unavailable native attack route from 1 to 5 seconds, and aligned the `GameHooks` negative-cache TTL to the same interval.
- This is a performance containment change for repeated failed signature/vtable validation; it does **not** claim to restore attacks. The native route still must be validated on the target runtime before dispatch is enabled.
- ProtoHax UserScript research confirmed that its `localPlayer.interactEntity(..., "attack")` helper queues a protocol/session-level entity interaction, whereas Bedaim currently dispatches through the in-process native GameMode virtual. The API cannot be copied directly without a compatible protocol/session layer.
- Experimental branch only; `main` remains untouched. Build and runtime tests are still required.

## 2026-10-19 — Public attack route accepts validated call thunks (experimental)

- Extended the ARM64 public `GameMode::attack(Actor&, Vec3 const&)` adapter recognizer to handle both a tail branch (`B`) and a normal call (`BL`) to the separately resolved four-argument implementation.
- A call/branch only counts when the wrapper first forwards the contact point from `x2` to `x3`, initializes the predictive-sound argument in `w2`, and the control-flow destination matches the resolved attack implementation (or a validated veneer to it).
- AutoHit continues to dispatch through the live public three-argument vtable entry; the internal method is used only as a validation anchor and optional result observer. No guessed vtable slot or unchecked native address is introduced.
- Added the internal/legacy signature addresses to route-failure logs to distinguish a missing public export from a missing helper signature.
- Experimental-only: the host/ARM64 build can validate source integration, but only a clean in-game test can verify that this runtime's public route is found and attack results are accepted.

## 2026-10-19 — AutoHit public attack ABI correction (experimental)

- Corrected the native attack dispatch to call the public `GameMode::attack(Actor&, Vec3 const&)` virtual with its three-argument ABI. The old resolver could select a different vtable entry because it matched a four-argument internal-helper-shaped signature; a `true` return from that route did not establish that the intended target was attacked.
- Added guarded ARM64 recognition for the public adapter that forwards the contact position and supplies the predictive-sound flag to its four-argument implementation. The resolver does not select a vtable entry solely because it resembles the internal helper.
- Preserved AutoHit's `GameModeActionEvent`, cancellable `AttackEvent`, and `AttackResultEvent` semantics without double-publishing when the optional internal observer is reached.
- The native result observer is now installed lazily on the validated adapter's actual branch target, instead of hooking the older ambiguous four-argument signature match at startup. Its trampoline therefore corresponds to the implementation called by the public virtual, and uninstall removes the dynamically installed handle safely.

- Video/log review: the Keystrokes HUD derives LMB CPS from internal `GameModeAction::Attack` events. It shows the attack path is being triggered, not a literal OS click or confirmed server damage. Latest logs showed native wrapper acceptance with target hurt time remaining at 0 before/after.
- Experimental branch only. Host regressions, ARM64 package build, and in-game server damage verification are separate checks; `main` and ElytraMacing remain untouched.

## 2026-10-19 — AutoHit contact-point and attack-feedback diagnostics

- Pass the nearest point on the freshly validated target AABB as the native attack contact position instead of the AABB center, aligning the supplied position with the reach geometry.
- Record the contact point and target hurt-time immediately before/after each native dispatch. Immediate hurt-time changes are diagnostic evidence only; server-authoritative target state may update later, so wrapper acceptance is not treated as confirmed damage.
- Inspired by ProtoHax UserScript's separation of target identity, server-side rotation, queued entity action and click pacing. The ProtoHax UserScript API is host-runtime-backed and cannot be copied literally into an in-process native module.
- Experimental branch only; requires ARM64 build and in-game confirmation. `main` and ElytraMacing remain untouched.

## 2026-10-19 — AutoHit staged pipeline rewrite (experimental)

- Reorganize AutoHit into explicit input, fresh-target, bounds, selected-weapon, reach, GameMode, validated attack-route, cooldown, and native-dispatch gates, following ProtoHax's separation of target selection from CPS pacing while retaining Bedaim's validated native GameMode route.
- Validate the public attack route before reserving a CPS slot, so unavailable GameMode/attack routes do not consume the cooldown. Recheck the slot under lock immediately before reserving it.
- Emit pipeline diagnostics only when the gate state changes; expected cooldown ticks are suppressed to avoid alternating accepted/cooldown records filling the rolling diagnostic buffer.
- Bind target snapshots to the current local-player pointer and revalidate the selected actor against the live engine actor list immediately before an attempt; refresh its AABB instead of dispatching against a stale cached pointer.
- Return early while the CPS cooldown is active, avoiding repeated target/inventory/GameMode checks on every player tick. Expected cooldown ticks do not emit diagnostic records.
- Native wrapper acceptance is explicitly distinguished from server-confirmed damage. No runtime success is claimed until in-game testing confirms actual hits and lag behavior.
- Kept the work on `rebuild/autohit-protohax-prelag-20261019`; `main` remains unchanged.

## 2026-10-18 — ARM64 tagged-pointer validation
- Normalize the top-byte tag only for `/proc/self/maps` range checks in `isReadableMemory`, preserving the original tagged pointer for actual memory access. This addresses Android ARM64 pointers such as `0xB4...` being incorrectly rejected as unreadable.
- Keep validation strict: the object and its vtable still need to reside in readable mappings, and ClientInstance vtable entries must still match the Minecraft image.
- Runtime confirmation is required; this is a focused follow-up to Build #734's `receiver_rejected` diagnostic.

## 2026-10-18 — AutoHit ClientInstance capture ABI fix
- Match the verified `bool ClientInstance::update(bool)` ABI and prefer its exact exported C++ symbol before falling back to the version-sensitive signature pattern.
- Validate the hooked receiver's readable object and vtable before caching it; rejected receivers no longer clear a previously verified local player.
- Add diagnostics that reveal whether the update detour actually runs, which hook route was installed, and why a receiver is rejected. Device runtime verification is still required.

# Changelog

## 2026-10-18 — AutoHit local-player resolution diagnostics
- Prefer an exact exported `ClientInstance::getLocalPlayer` symbol when available, then fall back to the validated vtable getter; the older ambiguous byte-pattern signature is not called.
- Add state-change diagnostics for null getter results, unavailable getter routes, invalid player candidates, and successful pointer capture.
- This is an isolated AutoHit recovery branch. It does not alter AimAssist's targeting algorithm; runtime testing is still required.

## 2026-10-17 — ElytraMacing module
- Added an isolated ElytraMacing module with its own configuration for elytra glide gating, minimum fall distance, mace equip/restore, target range, hitpoint height, crosshair tolerance, camera assist speed and attack interval.
- Glide-state reads use the current Bedrock entity-data encoding with SDK readable-memory checks. The module reads the existing BedrockAim target snapshot and does not write state to Aim Assist, Hitbox Expander, or MaceSwapper.
- The module is isolated on the feature branch and requires Android ARM64 build/runtime verification before being considered complete.
- All target/range state reads in the new module validate pointer-derived data locally; it does not change Aim Assist state or global hook policy.


## 2026-10-17 — Native symbol aliases and configurable ShiftBlade delay
- AutoHit/GameHooks now also checks the exact exported `$attack` wrapper symbol variants. Dispatch still requires an exact match against the active GameMode vtable entry; the native safety validation is unchanged.
- AutoTotem's offhand accessor and setter resolution now tries the matching exact exported `$getOffhandSlot`/`$setOffhandSlot` variants before the existing signature fallback. Returned pointers and post-transfer inventory state remain validated.
- ShiftBlade's sword retry interval is now configurable in local-player ticks (0–100; default 20). All existing target selection, ownership, restore, manual override, and consumption behavior is retained.

## 2026-10-17 — Diagnostics auto-save and combat failure visibility

- Changed rolling diagnostics auto-save interval to a configurable **1–300 seconds** (default 5 seconds); migrated older minute-based settings safely.
- Display the latest export result with local time, e.g. **Task completed successfully (09:19)**, plus the last known-good save time and path. A failed attempt no longer erases the last successful save information.
- Prefer exact exported Actor/Player/LocalPlayer offhand getter symbols before the generic signature-pattern fallback, and report which route was selected.
- Added a validated offhand-read result so AutoTotem distinguishes an unreadable native pointer from a genuinely empty offhand; it will not attempt a transfer based on an unknown slot state.
- AutoTotem diagnostics now include the invalid pointer/source, totem source-slot search, refill context, and remaining delay.
- An offhand getter that returns an unreadable pointer is quarantined after the first failure so the same untrusted native call is not repeated every tick; the log explicitly says when a restart is needed to retry.
- Revalidate the offhand pointer after the native setter and after source-item removal before inspecting the returned ItemStack, and include postcondition read failures in the transfer result.
- AutoHit now observes both native `Attack` and `UseItemAsAttack` action events as possible touchscreen repeat signals, and logs when those signals are observed, ignored, or arm the repeat window.
- GameHooks reports whether the GameMode and SurvivalMode `useItemAsAttack` hooks installed. AutoHit logs configured/held mouse buttons, touchscreen-repeat state, and remaining repeat-window time while waiting for input.
- These combat fixes remain on an isolated test branch; compile success does not establish runtime success. AutoHit/AutoTotem are not being merged into `main` by this change.

## 2026-10-17 — AutoHit touchscreen path and AutoTotem setter resolution

- Added a bounded, configurable touchscreen repeat window to AutoHit: a normal native attack event arms a short repeat period for mobile input paths that do not surface as launcher mouse-button events.
- Kept the existing mouse-held input path and made synthetic attacks explicitly ignore their own native attack events, preventing AutoHit from re-arming itself indefinitely.
- Added a toggle and 100–1000 ms repeat-window setting; the default window is 300 ms and is cleared when chat or a container opens.
- Enforced each weapon profile's CPS cap for failed as well as successful native attack attempts, so a rejected call cannot retry faster than the selected profile.
- Expanded AutoTotem's verified setter lookup to try the exact `Actor::setOffhandSlot` symbol before Player/LocalPlayer variants. No adjacent vtable slot is guessed; transfer still fails closed when the native API is unavailable.
- These changes remain experimental until an in-game test confirms the actual attack route and offhand postconditions on the target Bedrock/Levi runtime.

## 2026-10-17 — Diagnose AutoHit/AutoTotem runtime blockers

- Expanded native offhand-transfer diagnostics to name the exact missing or invalid prerequisite (offhand pointer, transaction manager, container vtable, or native symbols) instead of collapsing every failure into `native_api_unavailable`.
- AutoTotem now stops retrying every 100 ms after an unavailable or invalid native route; it safely waits for the module to be re-enabled or for a real offhand-state change.
- Added pointer/signature details when the native attack wrapper fails validation, without weakening the runtime safety guard or calling an unverified function.
- Log review: the provided session had no observed AutoHit mouse-input events after enable, and the public attack signature did not match the validated GameMode vtable entry. AutoHit still requires a follow-up fix for the actual mobile input path and matching runtime signature.
- These changes are isolated on the AutoHit/AutoTotem test branch; they are not merged into `main`.

## 2026-10-17 — Diagnostics autosave and AutoHit weapon profiles

- Added automatic TXT/JSON diagnostic exports to `/sdcard/Download/Bedaim/`, enabled by default while Module Diagnostics is enabled, with an immediate initial snapshot and a rolling autosave every minute by default.
- Kept file writes on a worker thread and retained manual timestamped exports plus export status in the diagnostics HUD.
- Added configurable per-weapon AutoHit profiles (enable/disable and CPS cap for swords, axes, maces and tridents) on the isolated experimental branch; unsupported/non-weapon items block attack attempts.
- Kept AutoHit and AutoTotem experimental; this branch does not promote either module to `main`.

## 2026-10-17 — Module Diagnostics

- Added **Module Diagnostics**, which writes `module_diagnostics.json` with registry and activation-state checks and shows a compact HUD summary.
- Reports duplicate IDs, invalid module metadata, empty registries, and activation-state inconsistencies.
- The report explicitly notes that generic checks do not prove game-time functionality; module-specific probes and in-game tests are still required.

## 2026-10-17 — BedrockAim, BedrockBounds and ShiftBlade integration

- Integrated the BedrockAim target-tracking updates and BedrockBounds native AABB expansion on a branch based directly on `main`, without merging the divergent experimental history.
- Added the **Ver Hitbox** preview and connected it to the existing hitbox renderer.
- Reduced Hitbox Expander AABB oscillation by keeping active expanded bounds stable between ticks, translating saved native bounds with actor movement, pruning stale/out-of-range targets, and restoring bounds when disabled or when target-type settings change.
- Added **ShiftBlade** with configurable target range and allowed height above stable ground. It now keeps a sword equipped while the target remains in range by default, yields to consumables and Mace Swapper priority, and offers **Respect manual hotbar changes** for users who prefer manual selection.
- Fixed a ForceGlobalRP packet-hook guard that checked the ResourcePacksInfo trampoline before calling the ResourcePackStack trampoline.
- Registered the existing **AutoReQ** module, which previously had source files but no registry entry.
- Fixed AutoReQ cooldown reservation so missing game-mode data, packet constructors, or sender prerequisites do not consume the requeue cooldown before a send can be attempted.
- Improved the legacy **FPS Unlocker** frame limiter: capped invalid/extreme FPS settings, guarded malformed config values, and rebased missed deadlines to avoid catch-up bursts.
- Fixed a Mace Swapper edge case where rapidly disabling and re-enabling during a pending swap could discard slot-restoration state; pending attacks are now cancelled and restoration is deferred safely to the next player tick.
- Kept AutoHit and AutoTotem out of this integration.
- Host regression tests and an Android ARM64 package build must pass on the final branch head before merging to `main`; in-game behavior still requires runtime testing.

## 2026-10-17 — Full diagnostics logs and ShiftBlade retries

- Added a bounded 4,096-entry diagnostic buffer supporting TRACE, DEBUG, INFO, WARN, ERROR and CRITICAL levels, with sequence numbers, timestamps, component tags, and newline-safe messages.
- Extended Module Diagnostics settings with **Export all logs now** (writes `bedaim_diagnostics.log` next to `config.json`) and **Clear in-memory logs**. The existing `module_diagnostics.json` health report remains available.
- Mirrored GameHooks logger output into the exportable log and instrumented module initialization/activation, runtime setup, config loading/writing failures, and ShiftBlade decision reasons.
- Added host tests covering all levels, snapshot, clear and export.
- ShiftBlade retries equipping a hotbar sword once per second while the target remains in range and the existing stable-ground requirement is met.
- Merely holding food no longer blocks ShiftBlade; it pauses for a short window after a successful native `UseItem` event while a consumable is selected.

## 2026-10-17 — Diagnostic probes and safe report export

- Added a state-change probe API that deduplicates repeated per-tick states without flooding the bounded log.
- Embedded the most recent 128 runtime log entries in `module_diagnostics.json` alongside registry checks, and clarified that the HUD warning/error counts refer to registry checks.
- Added host coverage for probe deduplication/reset and failed atomic export replacement; a failed write preserves the previous report.
- Extended BedrockAim diagnostics for missing native dependencies, target selection, and stale target snapshots; InventoryAccess reports offhand reader availability and transfer results.
- Added Mace Swapper reason-coded probes for fall thresholds, cooldown, target/crosshair/range gates, slot selection, pending attacks, and native attack results.
- Added `docs/MODULE_DIAGNOSTICS.md` with export instructions, log interpretation, and runtime-validation limits.


## Experimental AutoHit performance pass — 2026-10-19
- Cached ClientInstance vtable validation while the receiver/vtable identity remains stable, avoiding repeated module-range checks on the per-frame update hook.
- Bounded target selection to the 256 nearest entries per player tick and reduced AutoHit-only target acquisition to an 8-block search cube when Aim Assist is disabled.
- Limited AutoHit's live target revalidation to a small query radius derived from configured attack reach instead of rescanning the Aim Assist's full configured range.
- Treated the attack-input requirement as desktop-only on Android builds; enabling AutoHit on Android now activates automatically while retaining weapon, target, reach, cooldown, and native attack-route validation.
- This is a performance/activation hypothesis pending Android runtime testing. A successful compile or wrapper return does not prove server-confirmed damage or eliminate all FPS drops.

- Added bounded ARM64 branch-target validation for guarded public attack wrappers: a vtable entry is accepted only when its first 16 instructions contain a direct branch to the unique resolved attack signature. The validated entry is cached per GameMode/vtable identity.
- AutoHit now preflights the native attack route before requesting a target snapshot; AutoHit-only tracking is skipped while the build's native attack route is unavailable, preventing repeated expensive scans that cannot lead to an attack.


## AutoHit input-free activation — 2026-10-19
- Removed keyboard/mouse capture, held-input checks, and touchscreen repeat-window requirements from AutoHit. Enabling the module is now the only activation gate; target, weapon, range, cooldown, screen-state, and native-route validation remain.
- Removed obsolete attack-input controls from the AutoHit settings. The module's own keybind, if configured in the launcher, still only toggles the module and is not required to attack.
- Reduced unnecessary input-event subscriptions and state bookkeeping. The latest runtime logs still require a fresh test with AutoHit enabled: the most recent current-session log showed no AutoHit enable/dispatch events, and an earlier native trampoline result was false.

- Added disabled-module fast paths to MaceSwapper and Sword Swapper: when inactive and no slot restoration is pending, they skip player-position/inventory work and repeated per-tick diagnostics.


## 2026-10-19 — AutoHit no-feedback circuit breaker (experimental)

- Added a native-free, unit-tested guard that quarantines the currently selected actor after six consecutive native attack calls return accepted without an immediate positive local hurt-time transition.
- AutoHit stops dispatching against that same actor instead of continuing an unbounded stream of visually accepted but unconfirmed attacks. Switching target or toggling AutoHit resets the guard.
- The probe explicitly labels hurt-time as local evidence only: it is not proof of server-authoritative damage. This is a containment measure for the reported repeated-attack/disconnect symptom, not a claim that the missing-damage root cause is solved.
- The video also shows recurring Hitbox Expander bounds warnings and low FPS; those messages are distinct from the new AutoHit guard and still require the exported runtime logs/source path to diagnose precisely.
- Change remains on `perf/autohit-tick-throttle-20261019`; `main` and Aim Assist targeting logic remain unchanged. Host regression tests and Android ARM64 packaging must pass before distributing the new build.


## 2026-10-19 — AutoHit contact point respects server-side hitbox geometry

- When Hitbox Expander has enlarged an actor's client-side AABB, AutoHit now reads the saved original bounds, translates them by the actor's current movement, and computes the native contact point against that original box. The expanded box remains available for target acquisition, but no longer determines the attack contact coordinate.
- The original-bounds accessor refuses stale snapshots if the actor's AABB component or level changed, and falls back to the freshly validated live bounds rather than trusting a stale pointer-derived snapshot.
- This addresses a plausible mismatch between client-expanded geometry and server-validated hit coordinates; it still requires runtime confirmation, and it does not by itself prove that the server accepted damage.


## 2026-10-19 — GameMode wrapper validation recovery (experimental)

- The latest video shows the attack pipeline stopping before dispatch: `No validated public 3-argument attack route` and `no_validated_player_owned_candidate`. This explains why changing contact points alone could not make the current test deal damage.
- Extended safe, bounded validation of AArch64 tail thunks: public attack adapters now accept either value of the forwarded predictive-sound flag while still requiring the exact argument shuffle (Vec3* x2 → x3) and a direct tail branch, not a call.
- Public `useItem` vtable resolution now recognizes a direct thunk to the independently resolved method, which helps identify the player's actual GameMode instance without assuming a fixed vtable slot.
- Route misses are retried at a one-second cadence instead of being cached permanently for a stable object/vtable pair. This avoids a transient validation miss disabling resolution for the entire world session.
- The call path remains fail-closed: no direct call to the ambiguous four-argument internal method, no guessed vtable slot, and no weakening of server-side damage claims.
- This targets route discovery and the disconnect symptom's likely precursor (invalid dispatch), but actual hit registration and long-session stability still require in-game verification. `main` is unchanged.
