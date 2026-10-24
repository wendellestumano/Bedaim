# Connected Glass: source analysis and XRAY feasibility

## Provenance and source comparison

The Connected Glass implementation in Bedaim's `src/modules/visual/connectedglass.cpp` and `connectedglass.hpp` is byte-for-byte identical to the corresponding files in [QYCottage/BedrockTools](https://github.com/QYCottage/BedrockTools/tree/main/src/modules/visual). At the time checked (2026-10-24), the upstream repository's newest published release is v1.5.6; the checked source on `main` is not labeled as a published v1.5.7 release. Bedaim's package version is therefore tracked independently.

## How the module connects glass

The module does **not** replace block textures using a resource pack. It hooks Minecraft's native block tessellation/render path and changes how the glass texture's UV rectangle is sampled for each block face.

1. **Classifies the block.** `classifyGlass()` reads `Block::fullName()`, strips the `minecraft:` namespace, and recognizes glass blocks, stained glass, tinted/hard glass, glass panes and stained panes.
2. **Checks neighbors.** The `BlockSourceGetBlockForTessellation` signature is resolved and used to query adjacent block positions. Compatibility depends on block shape and the `connectDifferentColors` / `connectPanesToBlocks` settings.
3. **Chooses which edges to crop.** `getCropEdges()`, `shouldCropEdge()`, and face/edge settings decide whether a UV edge should be cropped based on a compatible neighbor or an exposed outer border.
4. **Changes UV coordinates safely.** `renderFace()` copies the `TextureUVCoordinateSet` using the native copy constructor/destructor signatures, modifies `mU0/mV0/mU1/mV1`, and passes the temporary texture to the original face renderer. The RAII `TextureCopy` wrapper avoids mutating a shared texture object in place.
5. **Handles panes separately.** Pane tessellation, background texture lookup, tessellator vertex emission and block render-face updates have dedicated detours. A `thread_local PaneRenderContext` tracks pane/face state while its vertices are emitted.
6. **Refreshes terrain after settings change.** The module resolves the render-chunk dirtying function and marks chunks dirty so the texture geometry is rebuilt when settings change.

The implementation installs hooks during `onInit()`; detours use an atomic enable flag to pass through to original code while the module is disabled. Preserve this pass-through if the hooks are refactored later.

## Which parts are useful for XRAY

Reusable ideas:

- A bounded block-name classifier that handles `minecraft:` names and variants.
- Native block-position neighbor queries through a version-resolved function.
- Per-render-call face state rather than retaining block pointers across frames.
- Hook installation with original-function fallbacks.
- Marking render chunks dirty when a visual option changes.

What Connected Glass **does not** do: it only changes the sampled UV edges for recognized glass blocks/panes. It does not make arbitrary blocks render through opaque walls, change world occlusion rules, or provide a general-purpose “show ores through terrain” mechanism. Reusing its UV-cropping logic alone would not implement XRAY.

## Recommended XRAY architecture

Build XRAY as a separate `XrayModule`, not as conditionals inside Connected Glass:

1. Start with an explicit allowlist (for example, selected ore block identifiers) and default unknown blocks to vanilla rendering.
2. Identify a rendering hook that can make decisions using current block identity/position and face, rather than only changing UV coordinates. Keep strict pass-through when XRAY is disabled.
3. Resolve and validate every signature for the exact supported Minecraft build. A signature mismatch or missing dependency must disable XRAY hooks, not call an uncertain native address.
4. On toggle or allowlist changes, queue affected render chunks for rebuilding instead of forcing a full-world rebuild every frame.
5. Test in a disposable local world: vanilla appearance with XRAY off, selected blocks visible with XRAY on, correct behavior at chunk boundaries, and no crash after repeated toggles/reloads.

The reusable model is the **block-aware render decision + chunk rebuild cycle**, not the connected-glass UV math. Keep XRAY isolated so its hooks cannot alter Connected Glass, ESP or Hitbox Expander.
