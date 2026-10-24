# X-Ray feasibility: Connected Glass and the supplied AlphaDepth pack

**Assessment date:** 2026-10-24  
**Result:** Connected Glass cannot safely be repurposed as the X-Ray implementation by itself.

## What Connected Glass actually does

`src/modules/visual/connectedglass.cpp` installs hooks for the six `BlockTessellator::tessellateFace*` directions, plus pane tessellation/vertex helpers. Its classifier deliberately recognizes glass blocks and panes. Its render path copies a `TextureUVCoordinateSet` and crops UV borders to connect adjacent glass; it does not load replacement textures, add an alpha-aware material pass, or switch the world's render material.

Changing the glass classifier to include stone or ores would therefore only apply the glass UV-cropping logic to unrelated blocks. It would not create X-Ray and risks damaging ordinary block rendering. Leave those hooks dedicated to Connected Glass.

## What the attached pack contains

The supplied `Outline_XRay_26.52_AlphaDepth_v1.0.6.mcpack` was inspected as an archive:

- Resource-pack manifest version **1.0.6**, minimum engine version **1.26.51**.
- **54** PNG block textures, using RGBA alpha for the outlined/transparent appearance.
- `renderer/materials/RenderChunk.material.bin` (**257,692 bytes**), with the `RenderChunk` and `DepthOnlyOpaque` markers expected by this pack.
- `blocks.json` remaps **22** ore/ancient-debris block shapes to `cauldron`.
- The manifest explicitly requires patched Minecraft/BetterRenderDragon with alpha-aware `Opaque` and `DepthOnlyOpaque` shaders.

This means the pack is not just a pile of textures. Its effect depends on the target renderer honoring that compiled RenderChunk material and the pack being loaded as a **Minecraft resource pack**.

## Integration constraints

Bedaim's `.levipack` packaging script currently emits the native library, `manifest.json`, icon, and font. Adding the `.mcpack` as an arbitrary extra ZIP member would not by itself install or activate it inside Minecraft. Also, the existing Connected Glass hooks do not provide a resource-pack activation or material-reload API.

There are two viable directions, which must not be conflated:

1. **Companion resource pack:** keep the supplied AlphaDepth pack installed/active in Minecraft, then implement a separate X-Ray module with its own block/render policy. Pack activation would remain separate unless a supported reload/selection API is proven.
2. **Fully integrated X-Ray:** first verify a runtime API for selecting/loading the pack and rebuilding RenderDragon materials/chunks on the exact patched Minecraft build, or implement a separate native rendering path that can safely provide equivalent alpha-aware rendering. Neither capability is established by the Connected Glass code.

Do not rewrite Connected Glass hooks into an X-Ray experiment, copy the compiled material blindly into the native library, or claim a hot-toggle works without checking the actual device/runtime. The attached pack's requirements make in-game validation mandatory.

## Pack validation helper

`scripts/validate_xray_pack.py` checks archive CRCs, manifest requirements, the compiled material markers, ore shape mappings, and alpha channels in block PNGs. It verifies file structure only; it does **not** prove the current Minecraft build can load the material or render the intended effect.

Run it with:

```sh
python3 scripts/validate_xray_pack.py /path/to/Outline_XRay_26.52_AlphaDepth_v1.0.6.mcpack
```
