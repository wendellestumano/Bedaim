# Bedaim

Bedaim is a native C++20 mod for Minecraft Bedrock on Android, built for LeviLauncher and Preloader. This fork keeps the Bedaim-specific gameplay and presentation modules rather than the inherited BedrockTools module collection.

## Retained Bedaim modules

- **AimAssist** — target tracking and aim configuration.
- **Mace Swapper** — Bedaim's mace combat workflow.
- **Sword Swapper** — Bedaim's sword swap workflow.
- **Unglide** — Bedaim's gliding-related input workflow.
- **Hitbox** — hitbox and related render diagnostics.
- **ESP** — player indicators and off-screen target indicators.
- **ULTRAKILL HUD** — custom combat HUD and feedback.
- **Module Diagnostics** — diagnostics and log export.

Shared runtime, launcher integration, configuration, event, and SDK code remains in place. Policy/math headers retained for tests are helpers, not registered launcher modules.

## Requirements

- Android 9 or newer
- 64-bit ARM device (`arm64-v8a`)
- [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid)
- Android NDK r28c, xmake, and Python 3 for building

## Build

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

The Android release build creates `libBedrockTools.so` and `Bedaim.levipack`. The package manifest's display name is currently blank/space-like for the launcher list; the package file itself remains named `Bedaim.levipack`.

## Install

1. Build or download the `Bedaim.levipack` artifact.
2. Import it using LeviLauncher's mod manager.
3. Launch Minecraft through LeviLauncher.

## License and credits

The repository includes upstream BedrockTools SDK/runtime components. Respect the licenses and credits of retained upstream code and follow Minecraft and LeviLauncher terms.

## Experimental ElytraMacing test branch

The `feat/elytramacing-from-scratch-20261026` branch contains an experimental ElytraMacing state machine and its Unglide/Key Mapper integration. Keep this branch separate from `main` until the Android ARM64 build and in-game tests are complete.

Before testing, configure the Key Mapper keymaps for start-glide (double jump), firework use, and recovery look-up; the stop-glide action uses the existing Unglide keymap by default. Test in a disposable world and verify each action independently: rocket use while gliding, glide start while falling, target-range mace/stop-glide transition, and recovery after Wind Burst or landing. A successful CI build confirms compilation and host policy tests only; it does not confirm that Android broadcasts or the full combat sequence work in-game.
