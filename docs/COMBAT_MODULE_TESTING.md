# Module Diagnostics

## Purpose

Module Diagnostics provides a compact registry health report and a bounded runtime log. It helps distinguish static registration problems from failures that occur only during gameplay; it does not claim that a module works merely because it initializes or compiles.

## Enable and export

1. Open the Bedaim module menu and enable **Module Diagnostics**.
2. Review the compact HUD summary for registry inconsistencies.
3. In its **Diagnostics** settings, use **Export all logs now** to write `bedaim_diagnostics.log` next to the configured `config.json`.
4. Use **Clear in-memory logs** only after exporting any entries you want to retain.

The separate `module_diagnostics.json` report contains the registry/metadata checks. The log contains timestamped entries with sequence numbers, levels (`TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `CRITICAL`), component names, and messages. The buffer is capped at 4,096 entries; export before clearing or before a long test if you need the full trace.

## Reading the log

- `Runtime`: signature resolution, hook installation, runtime startup.
- `ModuleRegistry`: module initialization and activation transitions.
- `BedrockAim`: required native dependency availability, target acquisition, and stale/missing target snapshots.
- `InventoryAccess`: availability of the offhand reader and result of a guarded offhand transfer.
- `MaceSwapper`: fall threshold, target/crosshair/range gates, slot selection, pending attack, and native attack result.
- `ShiftBlade`: target/range/ground gates, manual-slot overrides, consumable guard, and sword selection retries.

Repeated identical state probes are deduplicated, so a status that persists across hundreds of ticks does not flood the buffer. A new state is recorded when the reason changes.

## Limits

Diagnostics can identify which observable gate failed, but cannot prove that an internal game call has the intended in-game effect. Verify native hooks and inventory actions in a controlled test world on the exact Bedrock/Levi version. Do not merge experimental combat modules based only on a successful compile.

## Isolated combat-module test branch

The `test/autohit-autototem-diagnostics-20261017` branch contains experimental AutoHit and AutoTotem modules. They are deliberately not registered in `main`.

- **AutoHit** requires BedrockAim to be enabled, a fresh target, the target in the crosshair, a validated GameMode attack route, and the configured mouse button to be held. It never consumes normal mouse input and pauses while chat or container screens are open.
- **AutoTotem** only attempts a refill when the offhand is empty; it does not overwrite another offhand item and waits while a container screen is open. The native transfer returns a reason-coded failure instead of falling back to an unverified setter.
- Use the log probes to identify the first failed gate. A successful local setter/attack result still needs to be verified in-game and on a server before considering any merge.

The test branch exists for diagnosis and validation only. Keep these modules out of `main` until their runtime behavior is confirmed.
