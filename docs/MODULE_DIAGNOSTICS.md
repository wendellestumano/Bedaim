# Module Diagnostics

## Where the files go

All exports are written to the phone's shared Downloads folder, inside:

`/sdcard/Download/Bedaim/`

- Manual export: `bedaim_diagnostics_YYYYMMDD_HHMMSS_mmm.txt` or `.json` (each click creates a separate file).
- Periodic export: `bedaim_diagnostics_auto.txt` or `.json` (one rolling file is atomically overwritten each interval; the stale alternate-format rolling file is removed after a successful save).
- The health report `module_diagnostics.json` is still written beside the configured `config.json`, usually `/sdcard/games/BedrockTools/module_diagnostics.json`.

If Downloads export fails, the HUD status reports the failure and a reason is written to the in-memory diagnostics. On some Android configurations, shared-storage restrictions may prevent a native mod from writing to Downloads; check the device's storage permissions if the folder cannot be created.

## Enable and export

1. Open the Bedaim module menu and enable **Module Diagnostics**.
2. In its **Diagnostics** settings, choose **TXT** for readable text or **JSON** for structured data.
3. Press **Export all logs to Downloads now** to create a timestamped export.
4. To capture issues while playing, enable **Automatically export logs** and choose **Automatic save interval** from 1 to 300 seconds. The default is 5 seconds. Automatic export runs on a worker thread, not the game/render thread, and refreshes one rolling file.
5. Keep Module Diagnostics enabled while playing; disabling the module stops periodic export. Log collection itself remains active while the mod is running.
6. Before a new gameplay test, press **Start fresh gameplay log session**. This clears previous gameplay events, keeps essential startup facts (native attack-hook availability, offhand-reader readiness, module initialization), resets sequence numbers, and immediately replaces the rolling auto-save. The previous rolling file is replaced only after a successful atomic write; timestamped manual exports are preserved. If the save fails, the last known-good file remains available and the HUD reports the failure.
7. Use **Clear in-memory logs** only when you want to clear memory without starting/exporting a fresh session.

The full log contains timestamped entries with sequence numbers, levels (`TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `CRITICAL`), component names, and messages. The buffer is capped at 4,096 entries; automatic exports help preserve snapshots during longer sessions. The health report `module_diagnostics.json` contains registry/metadata checks plus the most recent 128 runtime log entries.

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


## Overlay layout controls

The Module Diagnostics HUD has independent layout controls in its Diagnostics settings. These do not change log collection, automatic-save cadence, session rotation, or archive retention:

- **Overlay X position / Overlay Y position** move the panel on screen.
- **Overlay width / Overlay height** resize the visible panel.
- **Overlay text size** scales the text.
- **Log page (newest first)** selects which page of the complete in-memory diagnostic log is shown. Entries are wrapped to the chosen width and are not truncated to a fixed character count; use the page slider to read the entire log buffer in manageable chunks.

The HUD currently uses settings-based repositioning rather than direct finger-drag gestures, because the available draw-command interface does not expose touch coordinates to the module. Position, dimensions, font size and page selection are saved in the existing module config. All original diagnostic/export settings remain unchanged.


Build follow-up: the first Android build caught a type mismatch in the wrapped log renderer (`levelName()` returns `std::string`). The renderer now stores the label as `std::string`; a fresh CI build is required to verify the fix.
