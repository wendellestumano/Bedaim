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
- The test is wired into GitHub Actions. Execution result is pending for this branch at the time this entry was written; no test pass is claimed yet.

### Build
- No local Android NDK build was executed in this tool session. GitHub Actions will provide the actual host-test and Android ARM64 package result after the branch is pushed and its workflow completes.

### Known Limitations
- The timer measures time since the module is enabled; disabling and re-enabling it resets the timer. It is not a persistent world/session clock and does not survive app restarts.
- HUD placement uses the existing `hudPosX`/`hudPosY` configuration convention; in-game layout and appearance still need validation on the target LeviLauncher/Bedrock version.
- This feature does not modify AutoHit, Auto Totem, hitbox behavior, or other combat modules.
