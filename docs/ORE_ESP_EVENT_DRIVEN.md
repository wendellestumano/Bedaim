# Ore ESP — event-driven outline prototype

## Design

The experimental `OreEspModule` draws colored wireframe boxes around ore blocks in a bounded neighborhood. It does not make stone/grass transparent and does not require a resource pack.

The module uses a cached, budgeted scan model inspired by the cache behavior observed in BedrockTools 1.5.7 and Apollon. Enabling the module queues an initial scan around the player. Each tick processes at most the configured number of block queries; ordinary render frames only read the cache and never query the world. Later local refreshes are queued after a confirmed block change, meaningful player movement, or scan-radius change.

A block-destruction action records the target position and its initial identity. The tick callback checks only pending target positions until the identity changes, then queues a neighborhood refresh. Nearby duplicate scan requests are coalesced. The cache is bounded at 4096 ores. Scan batches reconcile only the positions they visit, so previously cached ores in overlapping regions remain visible until their positions are rechecked; distant cache regions are pruned to leave room for new areas.

The module reuses the existing BedrockTools `BlockSourceGetBlock` signature and `Block::fullName()` wrapper. The shared `RenderLevel`/Tessellator renderer draws 12 wireframe edges per ore, batching geometry by ore kind. Line buffers are retained between frames, and per-kind buffers reserve capacity once based on the number of visible ores rather than reallocating for every ore.

## Defaults

- Horizontal scan radius: 16 blocks (configurable 4–32)
- Vertical scan radius: 8 blocks (configurable 2–16)
- Scan budget: 256 block reads/tick (configurable 64–1024)
- Maximum cached ores: 4096
- Maximum outlines submitted per frame: 384
- Coal outlines are disabled by default; other common ores are enabled

## Known constraints

- A default initial scan covers 33 × 33 × 17 = 18,513 block positions, spread across ticks (about 73 ticks at the default budget, excluding overhead). This is not a full-world scan.
- Movement-triggered scans begin after the player moves roughly half the configured horizontal/vertical radius and only when another scan is not already queued or running.
- This prototype confirms a mined block change by comparing its identity across ticks. It has not yet been verified in-game on every Minecraft/RenderDragon build.
- Rendering through walls and visual visibility depend on the resolved selection material and Minecraft render state. Successful compilation alone does not prove the outline is visible through opaque blocks; this must be tested in-game.
- If the native `BlockSourceGetBlock` signature is not resolved on a build, the module retries resolution and does not scan until the signature becomes available.
- A dimension/world change clears the cache. Chunk unload/reload behavior and long-distance cache retention still require live testing.

## Validation

- `xray_block_policy_test.cpp` compiled and passed on Termux.
- C++20 syntax checks passed for `hitbox.cpp`, `oreesp.cpp`, `ModuleRegistry.cpp`, `KeyMapperBridge.cpp`, `elytramacing.cpp`, `aimassist.cpp`, `autohit.cpp`, and `unglide.cpp`.
- A direct Termux syntax check of `GameHooks.cpp` could not run because the Android NDK `EGL/egl.h` headers are unavailable locally.
- A full Android ARM64 build/package and in-game visual test are still required before calling this feature stable.