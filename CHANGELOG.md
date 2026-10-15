# Changelog

### Improved
- AutoHit's actor query now covers at least its configured reach even when Aim Assist's detection range is lower; the scan adds a bounded margin for the expanded hitbox so an entity is not missed solely because its center falls outside the query cube. Expanded target bounds participate in acquisition, crosshair confirmation, and AutoHit's nearest-hit-point calculation.
- Hitbox Expander now uses bounded width/depth and height settings in blocks, preserves larger native boxes, anchors added height at the target's feet, migrates old multiplier settings, and clearly labels the geometry as client-side. This does not change server-authoritative collision.
- Inventory reads used by AutoTotem and swappers validate each native pointer region and the ItemStack counter/item/vtable chain before invoking the description-ID method.


## Combat Module Reliability — 2026-10-15

### Fixed
- Aim Assist V2 settings now rebuild the registered live schema values after accepted changes, preventing controls from retaining registration-time `current_value` snapshots.
- Configuration writes now use a separate atomic writer. A failed temporary-file rename no longer deletes the previous `config.json`; `ConfigManager::flush()` reports the last write result and failed writes are logged.

### Tests
- Added `atomic_config_writer_test` for first write, atomic replacement, temporary-file cleanup, and preservation of an existing destination after a failed replacement.

## Overnight Feature Development — 2026-10-15

### Added
- Added **Session Timer**, a new HUD module that displays elapsed time since it was enabled. It uses a monotonic clock, supports a configurable text size, background visibility/opacity, and a choice to always show hours. Files: `src/modules/hud/sessiontimer.hpp`, `src/modules/hud/sessiontimer.cpp`.
- Added a small, dependency-free duration formatting helper with explicit handling for negative input and sessions longer than 24 hours. File: `src/modules/hud/SessionTimerFormat.hpp`.
- Added host regression coverage for zero, negative, minute/hour/day boundaries, and compact formatting. File: `tests/launcher/session_timer_format_test.cpp`.

### Improved
- Registered Session Timer in `ModuleRegistry`, so it follows the existing module lifecycle and configuration persistence. File: `src/modules/ModuleRegistry.cpp`.
- Added the new module to the HUD inventory in `README.md` and the duration test to the CI workflow in `.github/workflows/build.yml`.

### Changed
- CI now builds and executes `session_timer_format_test` alongside the existing host regression tests. File: `tests/xmake.lua`, `.github/workflows/build.yml`.

### Research
- C++ `std::chrono::steady_clock` documentation: https://en.cppreference.com/w/cpp/chrono/steady_clock. Decision: measure elapsed time with a monotonic clock rather than wall-clock time, avoiding jumps if the system time changes.
- C++ `std::chrono::duration_cast` documentation: https://en.cppreference.com/w/cpp/chrono/duration/duration_cast. Decision: convert elapsed time to whole seconds explicitly before formatting.
- Preloader Android Mod Menu API: https://github.com/LiteLDev/preloader-android/blob/main/include/pl/ModMenu.hpp. Decision: render using the documented draw-command surface and register controls through the project's existing `ConfigSchemaBuilder` integration.

### Tests
- Added assertions for 0, 1, 59, 60, 3599, 3600, 86400, negative durations, and compact display mode.
- The Session Timer format test passed in GitHub Actions run #247 on source commit `838e9544197b602e3b5d0ed6817006d193b5be86`.

### Build
- GitHub Actions run #247 passed the host regression suite and Android ARM64 packaging for source commit `838e9544197b602e3b5d0ed6817006d193b5be86`; the resulting `BedrockTools.levipack` artifact was produced.

### Known Limitations
- The timer measures time since the module is enabled; disabling and re-enabling it resets the timer. It is not a persistent world/session clock and does not survive app restarts.
- HUD placement uses the existing `hudPosX`/`hudPosY` configuration convention; in-game layout and appearance still need validation on the target LeviLauncher/Bedrock version.
- This feature does not modify AutoHit, Auto Totem, hitbox behavior, or other combat modules.

## Overnight Feature Development — 2026-10-15 (continued)

### Added
- Added **Frame Stats**, a HUD module displaying current FPS, average frame time, average FPS, and 1% low over a rolling window. The sample window is configurable from 2–10 seconds; average FPS, 1% low, background, text size, and opacity are configurable. Files: `src/modules/hud/framestats.hpp`, `src/modules/hud/framestats.cpp`.
- Added a pure C++ rolling-statistics helper and regression test covering empty data, FPS calculations, slowest-1% calculation, invalid samples, pruning, non-monotonic timestamps, and reset. Files: `src/modules/hud/FrameStatsWindow.hpp`, `tests/launcher/frame_stats_window_test.cpp`.

### Improved
- Registered Frame Stats with the module registry and documented it in the README. Files: `src/modules/ModuleRegistry.cpp`, `README.md`.
- Added `frame_stats_window_test` to host test builds and CI. Files: `tests/xmake.lua`, `.github/workflows/build.yml`.

### Changed
- The module samples render callback cadence and does not add a Minecraft hook, signature, or version-specific offset.
- The 1% low is calculated as the reciprocal of the average frame time among the slowest 1% of retained samples; `std::nth_element` selects the slow tail without fully sorting each frame, the window is only pruned when its size changes or a sample arrives (including rejected stall samples), and the sample buffer is capped at 2,400 entries.

### Research
- C++ monotonic clock: https://en.cppreference.com/w/cpp/chrono/steady_clock — frame timestamps and elapsed windows use a monotonic clock.
- C++ partial selection: https://en.cppreference.com/w/cpp/algorithm/nth_element — `std::nth_element` selects the slowest 1% of frame-time samples without sorting the entire window every frame.
- Preloader Mod Menu API: https://github.com/LiteLDev/preloader-android/blob/main/include/pl/ModMenu.hpp — uses the established draw-command and V2 configuration-schema APIs.

### Tests
- Added host assertions for expected current FPS, mean frame time, mean FPS, 1% low, worst frame, ignored invalid values, time-window pruning, non-monotonic timestamps, and reset.
- `frame_stats_window_test` passed in CI run #247. The same run passed the Android ARM64 build/package step for source commit `838e9544197b602e3b5d0ed6817006d193b5be86`.

### Build
- CI run #247 successfully compiled and packaged the Android ARM64 `.levipack` with Frame Stats included. No in-game performance or visual validation is claimed.

### Known Limitations
- Frame Stats measures the cadence of enabled module render callbacks, not Minecraft server tick rate or an independently sampled GPU frame pipeline.
- 1% low is noisy for short windows with few samples.
- HUD appearance and position require validation in-game on the target LeviLauncher/Bedrock version.

## Overnight Feature Development — 2026-10-15 (continued, second module)

### Added
- Added **Distance Tracker**, a HUD module that accumulates player displacement while enabled. It supports horizontal-only or 3D distance, adjustable teleport/respawn discontinuity filtering, configurable text size, background and opacity. Files: `src/modules/hud/distancetracker.hpp`, `src/modules/hud/distancetracker.cpp`.
- Added a dependency-free `DistanceAccumulator` helper and host regression test covering horizontal and vertical movement, baseline initialization, teleport filtering, invalid coordinates, and reset. Files: `src/modules/hud/DistanceAccumulator.hpp`, `tests/launcher/distance_accumulator_test.cpp`.

### Improved
- Registered Distance Tracker through the existing module registry and documented it in the HUD list. Files: `src/modules/ModuleRegistry.cpp`, `README.md`.
- Added `distance_accumulator_test` to xmake host tests and the GitHub Actions workflow. Files: `tests/xmake.lua`, `.github/workflows/build.yml`.

### Research
- The existing Bedaim `LocalPlayerTickEvent` and `Player::position()` wrapper provide the required local position without new signatures or offsets. The module uses the existing event subscription, HUD draw-command, and V2 configuration-schema patterns.
- C++ floating-point validation: https://en.cppreference.com/w/cpp/numeric/math/isfinite — reject invalid coordinate samples instead of poisoning the cumulative total.

### Tests
- The Distance Accumulator test passed in CI run #247, covering horizontal/3D movement, teleport filtering, invalid-coordinate baseline reset, and threshold clamping.

### Build
- Android ARM64 build will run through the existing PR workflow; no in-game build or runtime validation is claimed yet.

### Known Limitations
- Distance Tracker resets when disabled/re-enabled and does not persist totals across world loads or app restarts.
- The helper cannot identify dimension/world changes by itself; the configurable per-tick jump threshold filters large discontinuities but cannot distinguish every teleport from unusually fast movement.
- Distance is based on client-observed player positions, not server-authoritative movement records.
