## 2026-10-21 — Break Indicator render-only isolation variant (experimental)

- Preserve all block interaction hooks and Break Indicator event/progress sampling, but skip HUD draw command creation and submission.
- Compare against the hooks-only variant and the combined isolation variant to determine whether frame rendering, hook interception, or both contribute to the block-break hitch/crash.
- AutoHit remains unchanged and lower priority. This is a diagnostic experiment, not a confirmed fix. Main is untouched.

# Changelog

## 2026-10-17 — Downloads export and periodic diagnostics

- Changed manual log export to create timestamped `.txt` or structured `.json` files in `/sdcard/Download/Bedaim/`.
- Added configurable automatic export, with a 1–60 minute interval (default 5 minutes); the rolling auto-export runs on a worker thread and overwrites one file to avoid unbounded storage growth.
- Added an export status/path to the diagnostics HUD and documented Android shared-storage permission failures.
- Added host coverage for structured JSON log serialization.

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
