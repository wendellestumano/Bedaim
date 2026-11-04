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
- **Book Expander** — imports notes from `Download/Bedaim/book_input.txt` or the module text field, creates a UTF-8-safe local plan of up to 200 pages, previews each page, provides Previous/Next navigation beyond page 50, and explicitly exports the complete plan to `Download/Bedaim/book_pages.txt`. It does not alter Minecraft's native book editor, packet validation, or server limits.

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


### Book Expander test build

The local page planner accepts up to 64 KiB of source text and can produce up to 200 page chunks (32–256 Unicode code points per chunk). The module includes a page preview, Previous/Next controls beyond page 50, and an explicit export to `Download/Bedaim/book_pages.txt` so the planned pages survive closing the game. To avoid using the Android keyboard while the native book screen is open, create or edit `Download/Bedaim/book_input.txt` and use **Load text from Downloads**. The page bodies are not written into rolling diagnostics automatically. This is strictly a local pagination/preparation capability. It does **not** modify Minecraft's native book editor, integrated server, multiplayer packet validation, or a remote server's book limits. Regression tests cover the 200-page cap, local cursor movement and export formatting.
