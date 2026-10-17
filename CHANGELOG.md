# Changelog

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
