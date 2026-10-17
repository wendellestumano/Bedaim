# Module Diagnostics

## Purpose

Module Diagnostics provides a compact registry health report and a bounded runtime log. It helps distinguish static registration problems from failures that occur only during gameplay; it does not claim that a module works merely because it initializes or compiles.

## Enable and export

1. Open the Bedaim module menu and enable **Module Diagnostics**.
2. Review the compact HUD summary for registry inconsistencies.
3. In its **Diagnostics** settings, use **Export all logs now** to write `bedaim_diagnostics.log` next to the configured `config.json`.
4. Use **Clear in-memory logs** only after exporting any entries you want to retain.

The `module_diagnostics.json` report contains the registry/metadata checks plus the most recent 128 runtime log entries. The log contains timestamped entries with sequence numbers, levels (`TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `CRITICAL`), component names, and messages. The buffer is capped at 4,096 entries; export before clearing or before a long test if you need the full trace.

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
