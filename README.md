# BedAim

BedAim is a standalone native LeviLauncher mod for Minecraft Bedrock on Android. It packages its own native library and exposes only BedAim's modules; it does **not** require the original BedrockTools package to be installed.

## Included modules

- **Combat / player:** Aim Assist, AutoHit, Hitbox Expander, Mace Swapper, Sword Swapper, Unglide
- **HUD / visual:** Game Mode Checker, Ultrakill HUD, ESP
- **Diagnostics:** Module Diagnostics
- **Additional custom module:** Swing Modifier

The upstream BedrockTools module registrations are intentionally excluded from BedAim's launcher menu. Shared runtime/SDK code remains in this source tree where BedAim modules still depend on it; it is compiled into BedAim's own library, not loaded from a separate BedrockTools installation.

## Build

Requirements: Android NDK r28c, xmake, and Python 3.

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

The build produces `libBedAim.so` and `BedAim.levipack` in the xmake target directory. Import `BedAim.levipack` through LeviLauncher's mod manager.

## Credits and license

BedAim's runtime/SDK foundation is derived from [BedrockTools](https://github.com/QYCottage/BedrockTools) by its upstream contributors. The project retains the upstream GPL-3.0 license and attribution; see [LICENSE](LICENSE). BedAim-specific modules and modifications are maintained in this repository.

Minecraft signatures and offsets are version-specific. A successful compile or package check does not prove runtime compatibility; test the generated package on the target Minecraft/LeviLauncher version.
