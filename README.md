# Bedaim

Bedaim is an independent native C++20 mod package for Minecraft Bedrock on Android through LeviLauncher/Preloader. It builds its own `libBedaim.so` and `Bedaim.levipack`; it does **not** require the original BedrockTools package to be installed or loaded beside it.

## Bedaim modules

- **Aim Assist** — target tracking and weapon-aware aiming configuration.
- **AutoHit** — configurable attack-input dispatch and weapon profiles.
- **Hitbox Expander** — adjustable target-bound expansion.
- **Mace Swapper** and **Sword Swapper** — weapon/slot coordination.
- **Unglide** — dedicated input/keymap route for leaving elytra flight.
- **ESP** — player outlines, target ring and off-screen direction indicators.
- **ULTRAKILL HUD** — custom combat/HUD effects.
- **Module Diagnostics** — runtime diagnostics and report export.

Only these Bedaim feature modules are registered in the launcher UI. The former BedrockTools feature modules are no longer built into the module registry. The RenderLevel renderer retained for ESP is an internal bridge, not an independent Hitbox feature. The spectator heuristic is an internal Aim Assist helper, not a GameMode Checker menu module. AutoTotem remains retired and is excluded.

## Build

Requirements: Android NDK r28c, xmake and Python 3.

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

The Android release target emits `libBedaim.so` and `Bedaim.levipack` in the XMake target directory. Import `Bedaim.levipack` through LeviLauncher’s mod manager.

## Source lineage and license

Bedaim keeps the compatible native wrapper, hook, Preloader, SDK and utility infrastructure required by its modules, based on the upstream [QYCottage/BedrockTools](https://github.com/QYCottage/BedrockTools) source. Feature registration, package branding, and Bedaim-specific modules are maintained here. The retained upstream source is covered by the repository’s MIT license; see [LICENSE](LICENSE).

The legacy `bedrocktools.*` internal module/config IDs are deliberately retained for backward compatibility with existing settings. The runtime library, Preloader mod registration, exported API symbol, package manifest entry and archive are branded for Bedaim.
