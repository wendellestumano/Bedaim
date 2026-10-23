## 2026-10-23 — Guard Unglide synchronized-data fallback reads

- Replace direct dereferences in the legacy gliding-state fallback with bounded reads through `/proc/self/mem`; invalid, unmapped, overflowed, or unreadable pointers now fail closed instead of risking a process crash.
- Keep the trusted native `Actor::getStatusFlag` lookup as the preferred fast path. If both the symbol and safe fallback are unavailable, suppress the Key Mapper request and emit `gliding_state_unavailable` rather than misreporting the player as grounded.
- Add host regression coverage for valid reads, low/null/zero/overflow inputs, and an unmapped address; register it in the Build workflow.
- CI and device compatibility validation are pending. On Android, if procfs access is restricted, the fallback intentionally fails closed.

## 2026-10-23 — Fix duplicate Unglide schema declaration

- Remove a duplicated `ConfigSchemaBuilder`, JSON snapshot, and value-reader lambda declaration in `UnglideModule::onMenuRegistered()` that caused the Android compile to fail with redefinition errors.
- Keep the existing single schema builder and settings callbacks unchanged. This is a compile-only fix; no gameplay behavior is intentionally changed.
- Validation pending PR CI on this experimental branch.

## 2026-10-23 — Unglide one-shot trigger and persistent settings path

- Changed Unglide from a repeated burst loop to a one-shot Space keymap request per target approach. A successful request latches the target and cannot repeat continuously while the player remains within range.
- Re-arms for the same target only after it separates beyond the configured trigger distance plus 1.5 blocks of hysteresis; a different valid target may trigger a new attempt.
- Requires the local player to be actively gliding, a fresh Aim Assist player target, a confirmed camera-ray/hitbox intersection, and the configured range/vertical constraints before dispatching Space through Key Mapper.
- Added a launcher-owned `Unglide Settings` page with explicit config-change callbacks as a durable alternative to the transient slider form. The original compact slider page remains available. The settings persistence behavior must still be verified on the device.
- Added policy tests for one-shot re-arm hysteresis. CI/device build validation for this latest change is pending; successful Android broadcast still does not prove that the keymap executes in-game.

## 2026-10-23 — Functional Key Mapper AutoHit baseline, per-item profiles, and crosshair-gated Unglide

- Used the AutoHit implementation associated with functional build run `39892141288` as the reference for the explicit Key Mapper broadcast route. AutoHit continues to dispatch through the configured Key Mapper keymap; this branch does not call the native GameMode attack wrapper.
- Kept the Weapon Profiles bench with Add Weapon, exact item-ID matching (including custom add-on namespaces), independent enabled state, Settings/Delete controls, per-item range/CPS, and a bounded initial burst. Legacy sword/axe/mace/trident settings are migrated into item profiles.
- Restored the functional Key Mapper cadence limit: at least 83 ms between requests (12 requests/second maximum). The burst CPS setting is also capped at 12 so its UI matches the maximum effective dispatch rate. Target/profile loss cancels a partial burst but does not clear the shared cooldown.
- Unglide now uses Aim Assist's fresh current target, requires it to be a player, and checks that the camera ray intersects that same target before sending Space through the dedicated Key Mapper keymap. The configured distance and vertical-difference gates remain in force; target loss, a different target type, or an off-target crosshair prevents dispatch.
- Added host tests for the Unglide crosshair gate and profile burst-rate normalization. Android runtime input delivery still requires an in-game device test; CI build verification is pending for this final commit.

- Extracted Unglide into its own module with independent Key Mapper dispatch, target distance/height settings, cooldown, burst state, diagnostics, and keybind; MaceFall no longer owns Unglide state or settings.
- Removed the unused MaceFall attack-interval setting/state (it was written but never consulted before attacks).
- ShiftBlade no longer exits early solely because MaceFall has claimed slam priority; it can continue its target/slot logic during combat.
- Unglide extraction into a standalone module remains a separate refactor to avoid mixing JNI/keymap state with MaceFall attack state.
- Unglide now uses a bounded three-request startup burst at 120 ms intervals, then returns to a 1-second steady cooldown; failed dispatches reset the burst and use a longer retry cooldown.
- Unglide no longer depends on the build-sensitive native Gliding flag; Key Mapper activation is now gated by configured target distance and vertical difference, with both measurements logged.
## Unglide Key Mapper dispatch
- Aligned the Unglide JNI/JavaVM lookup, local JNI frame, application-context fallback, explicit component broadcast, and error reporting with AutoHit while preserving the dedicated Unglide keymap UID.

## Unglide target gating
- Added a configurable maximum vertical difference between the gliding player and the nearest player target.
- Unglide now sends the Key Mapper keymap only when the target is within both the configured distance and height thresholds; diagnostic logs identify height-gate blocks.

## 2026-10-23 — Route Unglide through its dedicated Key Mapper keymap

- Replaced Unglide's MacroDroid Spacebar trigger with the supplied Key Mapper explicit receiver/action/UID-extra broadcast and keymap UID `e2225852-1df2-4232-bb26-606f11fc8745`.
- Reused AutoHit's JNI dispatch pattern: prefer the Preloader-cached JavaVM, retain dynamic VM discovery as fallback, build an explicit `ComponentName`, and pass the UID as a string extra.
- Kept the configured target distance and vertical-difference gates and dedicated keymap. The current implementation additionally checks Aim Assist's selected player and crosshair ray before dispatch; a successful Android broadcast confirms dispatch only, not in-game Space input.
- Android build and runtime validation pending CI/device testing.

## 2026-10-23 — Per-item weapon profiles, initial burst, and Unglide VM bootstrap

- Added a dynamic Weapon Profiles bench with Add Weapon, exact item-ID matching, per-item Switch/Settings/Delete controls, and a custom namespaced identifier field for add-on weapons.
- Migrated the previous sword/axe/mace/trident defaults into item-specific profiles on first load, while keeping old config fields for backwards compatibility.
- Added per-item attack range, steady normal CPS (up to 12), Initial Burst Hits, and Initial Burst CPS (up to 20). A burst starts when the held item or target changes, consumes only accepted Key Mapper broadcast requests, and switches to normal CPS after the final accepted burst request. Android acceptance is not server-confirmed damage.
- Added host regression assertions for custom ID normalization/matching and profile range/CPS/burst clamping.
- Cached the JavaVM supplied by Preloader and updated the input bridges to use it as a fallback when dynamic VM discovery is unavailable. Unglide now validates the current Aim Assist target and crosshair before its dedicated Key Mapper Space keymap; actual device delivery still requires runtime testing.
- Build and device validation are pending CI after this change. All work stays off `main`.

## 2026-10-22 — Key Mapper routing and steady AutoHit cadence

- Replaced the MacroDroid broadcast route with an explicit broadcast to Key Mapper's receiver, action, UUID extra and configured keymap UUID.
- Removed the Require Attack Input and Touchscreen Repeat controls from the UI; AutoHit checks target/weapon/range gates continuously while enabled.
- Reduced default rates to sword 5 CPS, axe 3 CPS, mace 1.5 CPS and trident 2.5 CPS; rate controls are capped at 12 CPS.
- Enforced an 83 ms minimum request interval and consume the interval on failed dispatches too, preventing unbounded retry bursts.
- Build/CI and in-game input delivery remain unverified. Successful broadcast dispatch does not prove Key Mapper emitted input to Minecraft.

## 2026-10-22 — Route AutoHit through MacroDroid keyboard key 0
- AutoHit now sends the package-scoped broadcast action `com.wendellestumano.bedaim.AUTOHIT_KEY0` to MacroDroid after its existing target, range, weapon-profile, input and cooldown gates pass. It no longer calls the native GameMode attack wrapper in this path.
- Configure MacroDroid with an enabled `Intent Received` trigger matching that action and an `Input Key Event` action for keyboard digit `0` (Android `KEYCODE_0 = 7`). MacroDroid's supported input path must send the key event to the foreground Minecraft/Levi Launcher game.
- A successful broadcast only confirms Android accepted the trigger; it does not confirm MacroDroid executed the action or Minecraft received key 0. Runtime testing and diagnostics remain required. This is rate-limited by the existing per-weapon CPS profile, not an unbounded key spam loop.

## 2026-10-22 — Restore experimental AutoHit only
- Re-registered AutoHit and included its implementation in the native build so it can be tested again; AutoTotem remains retired and excluded.
- This is a reactivation of the existing experimental code, not a claim that native attacks or input injection work at runtime. Validate the Android build and collect AutoHit diagnostics in-game before changing the attack route.
- No changes to `main`; work remains on `feature/elytra-aim-gamemode-checker-20261022`.

## 2026-10-22 — Fix MacroDroid Spacebar dispatcher build
- Fixed stale Key Mapper result-enum/function names left behind during the switch to the package-scoped MacroDroid broadcast; the JNI dispatcher and diagnostics now consistently use `AutomationBroadcastResult` and MacroDroid-specific result labels.
- Android build #1108 failed at compile time on those four unresolved identifiers. Host regression tests passed before the Android compile stage; rerun CI on this fix.
- MacroDroid delivery and actual Android Spacebar injection remain unverified until tested on the target device.

## 2026-10-22 — Android Spacebar Unglide via MacroDroid
- Unglide now sends a package-scoped Android broadcast with action `com.wendellestumano.bedaim.UNGLIDE_SPACEBAR`. MacroDroid's exported Intent Received capability can respond with its native Input Key Event action set to Space (KEYCODE_SPACE = 62). The active Unglide path no longer calls `Mob::jumpFromGround` or `Player::stopGliding`.
- Added guarded JNI access to the process JavaVM and application context, with throttled diagnostics for VM/context/Intent/broadcast failures. Successful `sendBroadcast` only confirms dispatch, not that a MacroDroid macro or privileged input route is enabled.
- One-time MacroDroid setup: create an enabled macro with an `Intent Received` trigger using the action above, and an `Input Key Event` action for Space. Configure MacroDroid's supported input path (Shizuku/ADB/root as available) so the event reaches Minecraft.
- Creative and Spectator classification remains evidence-based: exact GameType/native predicates take precedence; repeated no-damage hits and repeated invisibility/no-collision/flying/block-overlap signals are labelled as heuristics.
- ShadowHook experiment PR was closed; current builds remain on the Preloader hook backend.

# Changelog

## 2026-10-22 — Testable Aim Assist held-weapon classification

- Extracted item-identifier matching into a pure policy helper so weapon classification order can be verified with host tests without touching game memory.
- Fixed classification precedence: Crossbow before Bow, known custom melee classes before generic Axe matching, and Pickaxe/Shovel/Hoe as tools instead of axes.
- Preserved the sample-informed Ranged prediction policy: only Bow, Crossbow, and Trident use Ranged-only target lead; the Bedrock spear samples use piercing/kinetic components rather than a projectile component.
- The prior commit's CI passed host regression tests and Android ARM64 packaging. This new classifier change requires its own CI and runtime item-ID verification.
## 2026-10-22 — Aim Assist weapon classifier hardening

- Made the supported weapon classification order explicit: native item identifiers are checked for specific tool and ranged/melee classes before the generic axe fallback, so custom weapons with names containing “axe” do not accidentally bypass the other-melee classifier when more-specific matching applies.
- Kept the sample-driven Ranged prediction policy in a pure header and maintained the fail-safe rule that only Bow, Crossbow, and Trident use Ranged-only lead.
- Validation: upstream Build #1049 passed host regressions and Android ARM64 packaging for the base commit. This additional classifier adjustment must pass its own CI; in-game item identifier coverage remains required.

## 2026-10-22 — Sample-driven Aim Assist weapon policy

- Used the supplied Bedrock Samples v1.26.50.4 item definitions to correct Aim Assist: the "Ranged" prediction profile no longer leads Spears as if they were projectiles. The shipped wooden, copper, golden, stone, iron, diamond, and netherite spear definitions use piercing/kinetic weapon components and minecraft:is_spear tags.
- Moved held-weapon prediction policy into a pure header and added host regression checks for bow, crossbow, trident, spear, melee, and tool categories.
- Fixed the cached weapon enum upper bound so Tool remains Tool instead of being clamped into OtherMelee, which could apply the wrong per-weapon aim point and speed profile.

## 2026-10-22 — Retire AutoHit/AutoTotem and gate Elytra displacement

- Removed AutoHit and AutoTotem from module registration and excluded their implementation files from the packaged native library, restoring the prior build without those experimental modules.
- Elytra distance dislocation now requires Elytra Mode enabled, the local player actually gliding, and Sticky tracking. Actual glide state is no longer erased merely because the special Elytra-mode toggle is off.
- Fixed gliding detection to search the bounded synchronized entity-data vector for the flags item instead of assuming it is always the first item; added host regression coverage for the Elytra/flight/Sticky gates.

## 2026-10-22 — Elytra distance dislocation, Mace Unglide, and regression checks

- Added **Elytra Distance Dislocate Strength** to Aim Assist's **Special** category. While Elytra Mode is enabled and the local player is actually gliding, the aim point now shifts downward in proportion to full 3D target distance, with configurable strength and a strict clamp to the target's real bounds. Removed the old hard-coded 7–8 block-only offset.
- Kept Aim Assist active while gliding instead of returning before it can apply the Elytra-specific aim point.
- Added Mace Swapper **Enable Unglide**, **Unglide ground clearance**, and **Unglide target distance** controls in **Special**. Unglide requires a fresh Aim Assist target, actual local-player gliding, a target within range, and a detected solid floor within the configured clearance. Player::stopGliding is resolved dynamically; if the runtime does not export it, the action is skipped and diagnosed rather than writing guessed flags or offsets.
- Persisted and clamped the new settings, added a retry throttle and diagnostic reasons, and added host regression tests for Elytra dislocation math, Unglide gating, and native GameType normalization.
- Aim Assist now tries the trusted exported `Actor::isAttackableGamemode()` method first, matching the attackability check used by Apollon; if it is unavailable, the existing GameType getter/setter-cache path remains the fallback. No Apollon version-specific offsets were copied. Both native lookup paths still require confirmation against the device's current Minecraft binary.
- Fixed an Aim Assist configuration inconsistency: duplicate reads after the legacy m_hitboxDistanceGrowth migration overwrote the migrated value. The effective range now consistently supports 0.1–500, and the legacy slider bounds for hitbox growth and rotation speed match the supported 500/1000 maxima.
- **Validation pending:** run host regression targets, Android ARM64 package build, CI, and in-game tests. The pure GameType test verifies enum normalization only; it does not prove that the current game's getter or setter observer resolves at runtime. The Unglide native symbol also needs confirmation in the target runtime.


## 2026-10-17 — Restore known-good player tick dispatch

- Compared the supplied BedrockTools 1.5.7 `.levipack` with the current source. The working implementation publishes `LocalPlayerPreTickEvent` and `LocalPlayerTickEvent` directly from the `NormalTick` hook.
- The regressed implementation gated both events on a separately resolved `ClientInstance::getLocalPlayer` pointer. The supplied runtime log reports `client_instance_invalid` and `local_player_unavailable`, so that gate suppressed the events consumed by Aim Assist and Hitbox Expander.
- Restored the 1.5.7 event-dispatch behavior and removed the speculative local-player vtable fallback. `clientUpdateDetour` again only stores the client pointer and publishes the update event.
- Validation still required: host regression tests, Android ARM64 package build, and in-game confirmation that Aim Assist target updates and Hitbox Expander apply/restore callbacks run.

## 2026-10-17 — LeviLamina local-player interface lead

- Researched versioned LeviLamina client headers and found `IClientInstance::getLocalPlayer() const` plus the `ClientInstance` override at commit `917f0defa15ffb34599eb4522da1617a4a55b42b`.
- Added the exact source references to the research note as a concrete candidate for cross-checking the unresolved Bedaim getter signature.
- Did not apply a vtable-index workaround: that declaration alone does not prove ABI/index/layout compatibility with the current Android Bedrock build.

## 2026-10-17 — Bedrock Add-On and LeviLamina interoperability research

- Added `docs/BEDROCK_ADDON_LEVILAMINA_RESEARCH.md` summarizing official Mojang/Microsoft Add-On samples, Script API raycast and inventory patterns, and LeviLamina's C++ ScriptActor wrapper.
- Identified `getEntitiesFromViewDirection` as a behavioral reference for ray/AABB selection, not as a drop-in native function or validated attack route.
- Documented that Script API lifetime-managed entities/containers are not equivalent to the native client LocalPlayer/GameMode/offhand pointers used by Bedaim.
- Proposed pure C++ ray/AABB regression tests and version-matched local-player symbol research before native behavior changes.
- Build #619 succeeded, including host regression tests and Android ARM64 package creation; runtime functionality remains unverified.

## 2026-10-17 — Local-player tick regression diagnostics

- Recorded the confirmed regression from commit `12a9d6f`: the runtime's `ClientInstanceGetLocalPlayer` signature did not resolve, so the strict NormalTick identity filter suppressed local-player events consumed by Aim Assist and Hitbox Expander.
- Split the diagnostic states into `client_instance_missing`, `getter_signature_unresolved`, `getter_returned_null`, and `ready` instead of conflating a missing signature with a null return.
- Kept the identity filter fail-closed; no guessed vtable index, unverified native call, or arbitrary NormalTick actor is accepted as local player.
- The diagnostics also show that native attack/UseItemAsAttack hooks are unavailable. AutoHit still cannot prove an attack route; immediate crosshair attacks must not bypass target freshness, hitbox intersection, or per-weapon CPS limits.
- This is a diagnostic/safety correction on an isolated test branch, not a runtime fix for local-player lookup. A validated alternate source is still needed, followed by Android ARM64 build and in-game verification.

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
