# Changelog

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
- The test is wired into GitHub Actions. Execution result is pending for the original overnight branch; no test pass is claimed in advance.

### Build
- The original overnight branch has not yet been validated by a fresh CI run for this continuation. This feature adds a host-test target and uses the existing Android ARM64 build workflow.

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
- The 1% low is calculated as the reciprocal of the average frame time among the slowest 1% of retained samples; the sample buffer is capped at 2,400 entries.

### Research
- C++ monotonic clock: https://en.cppreference.com/w/cpp/chrono/steady_clock — frame timestamps and elapsed windows use a monotonic clock.
- C++ sorting: https://en.cppreference.com/w/cpp/algorithm/sort — frame-time samples are sorted to calculate the slowest-1% metric.
- Preloader Mod Menu API: https://github.com/LiteLDev/preloader-android/blob/main/include/pl/ModMenu.hpp — uses the established draw-command and V2 configuration-schema APIs.

### Tests
- Added host assertions for expected current FPS, mean frame time, mean FPS, 1% low, worst frame, ignored invalid values, time-window pruning, non-monotonic timestamps, and reset.
- CI results are pending for this commit. No test or build pass is claimed until GitHub Actions completes.

### Build
- The existing workflow will run the new host test and package the Android ARM64 `.levipack` if all steps succeed. No local Android build is claimed.

### Known Limitations
- Frame Stats measures the cadence of enabled module render callbacks, not Minecraft server tick rate or an independently sampled GPU frame pipeline.
- 1% low is noisy for short windows with few samples.
- HUD appearance and position require validation in-game on the target LeviLauncher/Bedrock version.
