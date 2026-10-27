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

## Render-hook disambiguation and overlay depth behavior

The Minecraft 1.26.52.3 ARM64 library contains two functions matching the previous short `RenderLevel` prologue. The hook expects a `LevelRenderer` instance whose `mLevelRendererPlayer` member is at offset `0x420`; the signature is extended with instructions that uniquely identify that candidate in the tested library. This was validated by scanning the local 1.26.52.3 APK: the updated pattern matched exactly one location, at file offset `0xB318478`.

Ore outlines are now expanded by 0.006 block units to avoid being coplanar with block surfaces. During ore mesh submission, the renderer temporarily disables depth testing and depth writes, then restores both states. This is intended to make the bright colored wireframe visible through opaque blocks; it still requires in-game confirmation with the active RenderDragon backend. Diagnostics now distinguish unresolved block queries, missing render materials, an entered renderer hook, and an empty/non-empty cache snapshot.


## Tessellator vertex signature for Minecraft 1.26.52.3

The former `TessellatorVertex` pattern did not match the ARM64 library packaged with Minecraft 1.26.52.3. The replacement pattern was tested against `lib/arm64-v8a/libminecraftpe.so` from the installed test APK and matched exactly one address: file offset `0xA76F048`. Disassembly of the matched routine and a nearby wrapper that loads a `Vec3` into floating-point argument registers confirm the expected ABI: `Tessellator*` in x0 and x/y/z in s0/s1/s2. Runtime diagnostics now report whether this signature actually resolved when Bedaim initializes.
