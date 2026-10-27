# BedrockTools 1.5.7 Hitbox — static analysis notes

## Source integrity

- Input archive: `BedrockTools.levipack` from Downloads.
- SHA-256: `7bd3daf1e9733ef01983f5d219a58c0b99a58a1fb5564b1b304f8aca31c3c3da`.
- Package version: `1.5.7`; native entry: `libBedrockTools.so`.
- The original archive was not modified. Analysis was performed on a private extracted copy in Termux.
- Native library: ELF64/AArch64, Android NDK r28c, dynamically linked, stripped but with many exported C++ symbols retained.

## Relevant symbols and behavior

- `HitboxModule::onInit()` at ELF virtual offset `0x16c0ec` resolves native signatures and stores the target used by the render hook.
- `HitboxModule::applyPatch()` at `0x16c324` installs a `pl::memory::hook` with replacement routine `0x16c378` and a slot for the original function.
- The replacement routine calls the original function, then performs entity-related checks and invokes helper routines. This strongly suggests a hook close to the native entity-render path, rather than a standalone `RenderLevel` overlay. The precise target signature and argument meanings are not fully established yet.
- The module contains the settings/field names `outlineThick`, `horizLineSpacing`, `vertLineSpacing`, `hitboxColor`, `outlineColorHex`, and `m_outlineThickness`.
- Other useful strings include `Hitbox` and `Displays hitboxes of entities.`

## Changes applied to Bedaim in this pass

- Reused persistent line buffers for actor boxes, target rings, eye lines, and look lines to reduce per-frame/per-actor heap allocations.
- Fixed the Ore ESP line-buffer growth pattern: instead of calling `reserve(current_size + 12)` for every ore (which can repeatedly reallocate), count ores by kind first and reserve the required capacity once per kind. This keeps the existing batched mesh submission model while reducing allocation/copy overhead.
- Kept the current `RenderLevel` renderer and signature resolution intact. The binary's per-entity hook should not be copied blindly: its signature IDs and ABI are specific to BedrockTools 1.5.7 and the Minecraft build it targets. A future native-hook port needs a verified pattern for the exact Bedrock version and runtime argument validation.

## Verification

- `xray_block_policy_test.cpp`: compiled and passed on Termux.
- `hitbox.cpp`, `oreesp.cpp`, and `ModuleRegistry.cpp`: C++20 syntax checks passed against the local project and cached package headers.
- A direct syntax check of `GameHooks.cpp` on Termux was blocked by the absence of Android NDK `EGL/egl.h`; this is an environment limitation, not a confirmed source error.
- No full Android ARM64 build or in-game rendering test has been completed in this pass.

## Termux tools

Already available: Clang/LLVM, binutils/readelf, radare2, Java, Python, and unzip. Added only `binwalk` and `capstone` for this analysis. Ghidra has not been installed because the readily available prebuilt distribution is desktop-oriented and would require additional work to run natively on Android ARM64.