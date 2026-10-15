# Changelog

## Runtime Compatibility + Prediction Follow-up — 2026-10-15

### Corrected
- Aim Assist now samples the local player's position over bounded tick intervals and computes a smoothed local velocity. Prediction therefore uses target velocity relative to actual player movement instead of subtracting an always-zero local velocity.
- Held-weapon classification is cached for 100 ms and refreshed immediately when the selected hotbar slot changes, reducing native ItemStack identifier lookups while preserving weapon-filter changes.
- Sword Swapper now validates its cached sword slot only every 350 ms instead of resolving the cached ItemStack identifier on every tick when it is not actively swapping.
- Tracking and prediction settings accept both schema values and visible menu labels (for example, `Adaptive`/`Adaptativo` and `Ranged weapons`/`Ranged`), avoiding silent fallback to defaults when a menu implementation serializes labels.
- AutoHit now rebuilds its floating Hold control when the mouse-button binding changes, and `button1`/`mouse1` can use the touch Hold control on Android instead of requiring a physical mouse. The touch control gates AutoHit without injecting an unrelated keyboard key.
- AutoTotem retries resolving trusted inventory-screen APIs instead of permanently caching a failed lookup during early initialization. This addresses runtimes where BedrockTools/LeviLamina support symbols become available after module startup.
- AutoTotem now waits for a container-open event (up to 600 ms) before beginning the equip step, then requests close after transfer. If the runtime does not report an open event, it falls back to the verified transfer path instead of getting stuck in the UI state machine.
- EGL frame events can no longer suppress the original `eglSwapBuffers` call. Frame events remain observable for HUD updates, but one module's cancellation cannot freeze rendering or block BedrockTools' renderer.

### Validation
- Host regression tests and Android ARM64 packaging are being rerun on this revision.
- Live FPS, typed AutoHit input, visible AutoTotem open/equip/close behavior, and in-game HUD/swappers still require testing in Minecraft Bedrock 26.52 with the target LeviLamina/BedrockTools build.

## Input, Aim Profile, HUD & Compatibility Pass — 2026-10-15

### Corrected
- AutoHit accepts typed bindings: `button1`/ `mouse1` observe a physical mouse button without consuming it; `shift`, digits, letters and `keycode:N` support keyboard input. Legacy numeric configs remain readable, while partial text edits preserve the last valid binding.
- Weapon filtering classifies the held item from one identifier lookup and explicitly excludes pickaxes from the axe family. Filtering now gates steering/AutoHit without erasing the target snapshot used by Sword and Mace Swapper.
- Aim profile affects both Sticky and Smooth/Adaptive tracking. Rotation Speed accepts up to 200 in both the V2 and legacy menus and the runtime clamp.
- Mace Swapper obtains the player's own validated GameMode and checks the attack route before changing slots, rather than depending on a GameMode observed only after a separate action.
- AutoTotem's inventory opener now also tries a guarded, Minecraft-image-validated Player vtable route when the exact open symbol is unavailable; close still requires the exact trusted runtime helper. If no valid open/close pair exists, it explicitly falls back to the verified native inventory transfer rather than claiming that a visible inventory sequence occurred.
- ULTRAKILL HUD no longer cancels global BedrockTools FrameEvents to emulate hit-stop. The impact hold is visual-only; HUD config uses synchronized snapshots and draw-command updates are paced to 30 Hz. The held-item identifier refreshes only on hotbar change or every 250 ms rather than on every player tick.

### Performance
- Aim Assist render frames reuse the latest valid target snapshot rather than rescanning the nearby-actor vector; AutoHit also reuses the target selected in the same player tick instead of doing a second large native actor scan.
- Sword Swapper caches a validated sword slot and throttles no-sword hotbar scans. Aim Assist weapon classification avoids repeated native ItemStack string resolution. AutoTotem retains slower bounded inventory/offhand polling on mobile.
- Target prediction now factors the local player's measured velocity as well as the target's velocity, with bounded sampling and lead clamps.

### Validation status
- Added regression coverage for the AutoHit key alias parser. GitHub Actions is validating the current branch; gameplay FPS, the visible AutoTotem UI sequence and in-game module behavior still require a live test on the target Bedrock/LeviLamina version.

## Combat Lag Rollback + Input/Inventory Controls — 2026-10-15

### Corrected
- Restored `src/modules/player/aimassist.cpp` and `aimassist.hpp` to the implementation from pre-performance commit `907ccdc265a25743b2d7de42a02612e40b6b5b1a`, before the render-frame snapshot rewrite. This is a deliberate rollback of the newer Aim Assist hot-path changes after the user reported severe stutter; the near/far hitbox curve and new key controls are then layered on top.
- Sword Swapper now exits immediately when it already owns the selected sword slot, avoiding repeated ItemStack identifier resolution and hotbar scans on every player tick. It reuses the observed selected-slot value and only resolves the held item identifier immediately before a real swap.
- AutoTotem uses a 150 ms minimum offhand polling interval and 400 ms minimum full-inventory retry when no totem is found. This reduces expensive native ItemStack string lookups on low-end Android while keeping the UI-step delay configurable.

### Added
- **AutoHit key option:** Android key-code setting; e.g. Android `KEYCODE_0` is code 7. A floating Hold button can emit the selected key and gate native AutoHit while held. Default 0 preserves automatic native AutoHit. The public launcher API observes physical keys and supports floating buttons, but does not provide generic programmatic injection of arbitrary Android key events.
- **AutoTotem inventory UI sequence:** resolves the native `LocalPlayer::openInventory()` (or inherited `Player::openInventory()`) from `libminecraftpe.so` and the exact `LocalPlayer::closeContainerScreen(ContainerType::Inventory)` helper from a trusted loaded Bedrock/LeviLamina runtime. If both are available, it opens inventory, waits the configurable UI-step delay, equips via the existing verified native transaction, then requests inventory close. If the symbols are missing, a guarded Player vtable fallback is attempted only after checking the candidate and neighboring virtual-function addresses belong to `libminecraftpe.so`; if no compatible open/close pair is available, the module falls back to the existing verified transaction. UI-step delay defaults to 80 ms, range 0–500 ms.
- **Distance Hitbox Strength curve:** the virtual steering box is under native bounds up close and grows smoothly with distance. Default strength reaches roughly 2.8x at the far end of the configured range (bounded to 12x), while the target aim point remains constrained to native target bounds.

### Validation
- New helper tests cover near-to-far monotonic behavior, non-finite inputs, disabled mode, bounds, offhand polling, scan retry and UI step delay.
- GitHub Actions run #261 passed the complete host regression suite and Android ARM64 build/package for commit `322d6307522c37fa73c5baa11f633258097ea7c7`: https://github.com/wendellestumano/Bedaim/actions/runs/38755547172. No in-game FPS measurement or runtime symbol-availability verification has been performed yet.

## Combat Hot-Path Optimization — 2026-10-15

### Improved
- Aim Assist refreshes actor candidates at the game-tick cadence and steers from the last validated bounds snapshot on render frames; render frames no longer scan the native actor vector. AutoHit now reuses the just-validated same-tick target snapshot instead of requesting/scanning the entire native actor vector a second time.
- Weapon classification resolves the held ItemStack identifier once per tick, then caches weapon family, naturalization applicability and aim multiplier for render-frame steering.
- Aim Assist stops walking the distance-sorted actor list once entries exceed the useful scan radius, before making more native position/bounds calls; it also applies cheap range/geometry filtering before name-tag, invisibility and voxel line-of-sight checks.
- Inventory memory validation reuses a short-lived readable-memory map snapshot instead of reopening and scanning /proc/self/maps for each pointer check.
- AutoHit first prefers GameMode observed from a real callback; when none is available, it can recover the Player-owned GameMode only after validating readable field storage and an exact match between vtable slot 16 and the resolved GameMode/SurvivalMode attack signature. Unverified pointers remain rejected.
- AutoTotem evaluates delay, container and combat gates before scanning inventory slots, and skips Aim Assist target queries when Combat Only is disabled.

### Supplied binary inspection
- The uploaded libReverseSigma.so is an Android AArch64, stripped Apollon JNI library. Strings such as hitBoxValue, hitBox and emulateNativeAttack do not identify a reusable native hitbox function or provide its ABI; no offsets or machine code were transplanted.
- The supplied binary was inspected statically only and is not loaded or executed by Bedaim.

### Validation
- GitHub Actions run #258 passed the complete host regression suite and Android ARM64 build/package for commit `68871663d446e417143843635761276abe80dc43`: https://github.com/wendellestumano/Bedaim/actions/runs/38748534759. In-game FPS and AutoHit/AutoTotem behavior still require live validation.

## Combat Core Reliability — 2026-10-15

### Fixed
- Serialized complete configuration snapshot creation and queue insertion in `ConfigManager::save()`. Concurrent menu/toggle callbacks can no longer enqueue an older snapshot after a newer one and silently revert recently changed Aim Assist settings.
- AutoHit now revalidates its cached target against a fresh native actor snapshot before calling actor methods or the player-type callback. If the target despawned or the native vector metadata is invalid, the attack is skipped and throttled instead of dereferencing a stale entity pointer.
- AutoHit uses the live target bounds from the validated snapshot before applying the configured client-side Hitbox Expander geometry.

### Validation
- Pending GitHub Actions host regression tests and Android ARM64 build for this change. These changes do not establish server-authoritative hitbox expansion; the server still validates hits and reach.

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
