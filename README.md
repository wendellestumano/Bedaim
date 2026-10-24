# Bedaim

## Introduction

Bedaim is an independent native C++20 module for Minecraft Bedrock on Android, built for LeviLauncher and Preloader. It focuses on Bedaim-owned combat and visual features. The shared SDK, signatures, hooks, and event infrastructure remain compatible with the BedrockTools-derived source layer.

The source is public so Bedaim features and native rendering behavior can be studied, tested, and extended.

## Features

- Native C++20 mod built for LeviLauncher and Preloader
- Bedaim-owned combat, HUD, and visual modules
- Connected Glass retained as a native-rendering research module
- Public headers for Minecraft wrappers, offsets, signatures, and utilities
- Typed event system with runtime subscriptions for other native mods
- LeviLauncher mod-menu integration and persistent configuration
- Open-source and designed to be practical to extend

## Modules

**Visual:** ESP (player outlines and off-screen indicators), Connected Glass

**HUD:** ULTRAKILL HUD

**Combat:** Aim Assist, AutoHit, Mace Swapper, Sword Swapper, Unglide, Hitbox Expander

**Diagnostics:** Module Diagnostics

## System Requirements

- Android 9 or newer
- 64-bit ARM device (`arm64-v8a`)
- [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid)
- A Minecraft Bedrock version supported by the Bedaim build you are using

## Installation

1. Install LeviLauncher.
2. Download the latest `Bedaim.levipack` release.
3. Import the package from LeviLauncher's mod manager and enable it.
4. Launch Minecraft through LeviLauncher.

## Development Setup

Requirements:

- Android NDK r28c
- xmake
- Python 3

Build for Android ARM64:

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

The release build produces `libBedaim.so` and `Bedaim.levipack` in the xmake target directory.

Public SDK headers are under `include/bedrocktools`. Shared runtime code lives under `src/core`, while features are kept under `src/modules` by category. Minecraft signatures and offsets are version-specific, so those are the main pieces that normally need updating for a new game build.

Example event subscription from another native mod:

```cpp
#include <bedrocktools/BedrockTools.hpp>

bedrocktools::events::RuntimeListener<bedrocktools::events::LocalPlayerTickEvent> listener(
    [](auto& event) {
        if (!event.player) return;
        auto position = event.player->position();
    }
);
```

## Contributing

Pull requests are highly appreciated. Keep changes focused, preserve the existing project structure, and test changes against the intended Minecraft version before submitting them.

## Usage Guidelines

Do not use LeviLauncher or BedrockTools to violate Mojang or Microsoft's user agreements.

**Disclaimer:** The authors and contributors of BedrockTools and LeviLauncher are not responsible for bans, damages, or issues arising from the use of this software. Use it at your own risk and in accordance with Minecraft's terms of service.

## Credits & Acknowledgements

BedrockTools is made by [RadiantByte](https://github.com/RadiantByte).

Special thanks to [dreamguxiang](https://github.com/dreamguxiang) for helping make this mod possible.

Motion blur module based on [mcpelauncher-motion-blur](https://github.com/CrackedMatter/mcpelauncher-motion-blur) by [CrackedMatter](https://github.com/CrackedMatter).

Thanks to [Kashifro](https://github.com/Kashifro) for the Shulker Preview and Tablist modules.

Built for [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid).

## Contact

Discord: [discord.gg/rMgdpTFFVg](https://discord.gg/rMgdpTFFVg)

**Report Issues:** Open an issue in this repository.

## License

BedrockTools is licensed under the [GNU General Public License v3.0](LICENSE).
