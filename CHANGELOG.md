# Changelog

## Overnight Feature Development — 2026-10-15

### Added
- Added **Session Timer**, a new HUD module that displays elapsed time since it was enabled. It uses a monotonic clock, supports a configurable text size, background visibility/opacity, and a choice to always show hours. Files: `src/modules/hud/sessiontimer.hpp`, `src/modules/hud/sessiontimer.cpp`.
- Added small, dependency-free formatting helpers for session duration and movement speed, including safe handling of negative/non-finite speed values and clamped precision. Files: `src/modules/hud/SessionTimerFormat.hpp`, `src/modules/hud/SpeedDisplayFormat.hpp`.
- Added host regression coverage for duration boundaries, compact formatting, speed rounding, precision limits, negative values, infinity, and NaN. Files: `tests/launcher/session_timer_format_test.cpp`, `tests/launcher/speed_display_format_test.cpp`.

### Improved
- Registered Session Timer in `ModuleRegistry`, so it follows the existing module lifecycle and configuration persistence. File: `src/modules/ModuleRegistry.cpp`.
- Exposed Speed Display's existing 3D movement mode in the settings UI and added configurable 0–2 decimal-place output. The selected mode and precision are now persisted and clamped when loading configuration. Files: `src/modules/hud/speeddisplay.hpp`, `src/modules/hud/speeddisplay.cpp`.
- Added the new module to the HUD inventory in `README.md` and the duration test to the CI workflow in `.github/workflows/build.yml`.

### Changed
- CI now builds and executes `session_timer_format_test` alongside the existing host regression tests. File: `tests/xmake.lua`, `.github/workflows/build.yml`.

### Research
- C++ `std::chrono::steady_clock` documentation: https://en.cppreference.com/w/cpp/chrono/steady_clock. Decision: measure elapsed time with a monotonic clock rather than wall-clock time, avoiding jumps if the system time changes.
- C++ `std::chrono::duration_cast` documentation: https://en.cppreference.com/w/cpp/chrono/duration/duration_cast. Decision: convert elapsed time to whole seconds explicitly before formatting.
- Preloader Android Mod Menu API: https://github.com/LiteLDev/preloader-android/blob/main/include/pl/ModMenu.hpp. Decision: render using the documented draw-command surface and register controls through the project's existing `ConfigSchemaBuilder` integration.

### Tests
- Added assertions for 0, 1, 59, 60, 3599, 3600, 86400, negative durations, and compact display mode.
- The Session Timer formatting test passed on GitHub Actions run #234 before the Speed Display follow-up was added. Speed Display formatter tests are now also wired into CI; a fresh run is needed to validate the complete branch.

### Build
- No local Android NDK build was executed in this tool session. GitHub Actions is building the branch; the final Android ARM64 package result is pending.

### Known Limitations
- The timer measures time since the module is enabled; disabling and re-enabling it resets the timer. It is not a persistent world/session clock and does not survive app restarts.
- HUD placement uses the existing `hudPosX`/`hudPosY` configuration convention; in-game layout and appearance still need validation on the target LeviLauncher/Bedrock version.
- This feature does not modify AutoHit, Auto Totem, hitbox behavior, or other combat modules.
