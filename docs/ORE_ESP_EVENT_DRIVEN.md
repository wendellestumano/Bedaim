# Ore ESP — event-driven outline prototype

## Design

The experimental `OreEspModule` draws colored wireframe boxes around ore blocks found in a bounded neighborhood. It does not make stone/grass transparent and does not require a resource pack.

The module follows an event-driven cache model: a block-destruction action records the target block position and its initial identity. The tick callback checks only those pending positions until the block identity changes, then queues one neighborhood scan. That scan is processed in small batches, with duplicate nearby scans coalesced and cache size bounded. Ordinary render frames read the cache and do not perform world block queries.

The existing BedrockTools `BlockSourceGetBlock` signature and `Block::fullName()` wrapper are reused. The existing `RenderLevel`/Tessellator path from the visual hitbox renderer draws the 12 wireframe edges, batching geometry by ore color. Geometry buffers are retained between frames to reduce repeated allocations.

## Defaults

- Horizontal scan radius: 16 blocks (configurable 4–32)
- Vertical scan radius: 8 blocks (configurable 2–16)
- Scan budget: 256 block reads/tick (configurable 64–1024)
- Maximum cached ores: 4096
- Maximum outlines submitted per frame: 384
- Coal outlines are disabled by default; other common ores are enabled

## Known constraints

- The module begins with an empty cache and waits for a block change; it does not perform an expensive full-area scan merely because the module was enabled.
- This prototype confirms a block change by comparing the block identity at the targeted position across ticks. It has not yet been verified in-game on every Minecraft/RenderDragon build.
- Rendering through walls and visual visibility depend on the currently resolved selection material and Minecraft render state. Successful compilation alone does not prove the outline is visible through opaque blocks; this must be tested in-game.
- If the native `BlockSourceGetBlock` signature is not resolved on a build, the module logs the missing dependency and does not scan.
- A dimension/world change clears the cache. Chunk unload/reload and long-distance stale-cache behavior still require live testing.

## Validation

The host policy test checks ore ID recognition and color mapping. Local syntax checks cover `GameHooks.cpp`, `ModuleRegistry.cpp`, `hitbox.cpp`, and `oreesp.cpp`. CI Android ARM64 packaging and in-game tests are required before calling this feature stable.
