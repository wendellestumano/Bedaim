# Module Diagnostics

## Where the files go

All exports are written to the phone's shared Downloads folder, inside:

`/sdcard/Download/Bedaim/`

- Manual export: `bedaim_diagnostics_YYYYMMDD_HHMMSS_mmm.txt` or `.json` (each click creates a separate file).
- Current gameplay: `bedaim_diagnostics_auto.json` (contains only the current gameplay/test session; atomically overwritten on each auto-save and reset when Module Diagnostics is enabled or **Start fresh gameplay log session** is used).
- Lifetime total: `bedaim_diagnostics_total.json` (cumulative, deduplicated entries from every gameplay session; updated on each successful current-session auto-save).
- Manual export: `bedaim_diagnostics_YYYYMMDD_HHMMSS_mmm.txt` or `.json` (each click creates a separate file; the format selector applies to manual exports only).
- The health report `module_diagnostics.json` is still written beside the configured `config.json`, usually `/sdcard/games/BedrockTools/module_diagnostics.json`.

If Downloads export fails, the HUD status reports the failure and a reason is written to the in-memory diagnostics. On some Android configurations, shared-storage restrictions may prevent a native mod from writing to Downloads; check the device's storage permissions if the folder cannot be created.

## Enable and export

1. Open the Bedaim module menu and enable **Module Diagnostics**.
2. In its **Diagnostics** settings, choose **TXT** or **JSON** for timestamped manual exports. The two rolling files are always JSON.
3. Press **Export all logs to Downloads now** to create a timestamped export.
4. To capture issues while playing, enable **Automatically export logs** and choose **Automatic save interval** from 1 to 300 seconds. The default is 5 seconds. Automatic export runs on a worker thread, not the game/render thread, refreshes the current-session JSON, and merges new entries into the lifetime total without duplicating entries.
5. Keep Module Diagnostics enabled while playing; disabling the module stops periodic export. Log collection itself remains active while the mod is running.
6. A fresh session starts automatically when Module Diagnostics is enabled. You can also press **Start fresh gameplay log session** before a test. The previous current-session JSON is merged into the lifetime total before it is deleted and recreated; entries are deduplicated, so periodic saves do not pile up duplicates. If archival fails, the reset is aborted to protect the previous file. Essential startup facts (native attack-hook availability, offhand-reader readiness, module initialization) are kept in the new session, sequence numbers reset, and timestamped manual exports remain untouched.
7. Use **Clear in-memory logs** only when you want to clear memory without starting/exporting a fresh session.

The full log contains timestamped entries with sequence numbers, levels (`TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `CRITICAL`), component names, and messages. The in-memory buffer is capped at 4,096 entries; the current-session file reflects the active session, while the lifetime file accumulates deduplicated entries across sessions. The health report `module_diagnostics.json` contains registry/metadata checks plus the most recent 128 runtime log entries.

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
