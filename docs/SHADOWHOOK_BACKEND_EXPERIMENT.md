# ShadowHook backend experiment

This branch adds a runtime-selectable backend to the existing `bedrocktools::hooks`
adapter. On Android it tries to find and initialize the public ShadowHook native API
(`shadowhook_init`, `shadowhook_hook_func_addr`, and `shadowhook_unhook`). If no
usable `libshadowhook.so` is available, it keeps the existing Preloader hook backend.

## Important limitations

- This is an opt-in-by-availability experiment, not proof that ShadowHook is bundled
  with every LeviLauncher/Apollon setup. The current `.levipack` package contains only
  `libBedrockTools.so`, the icon, and the font; it does not bundle `libshadowhook.so`.
- If ShadowHook is present and initializes successfully, all hooks installed through
  this adapter use it. A failed ShadowHook hook returns failure; it is not silently
  retried through Preloader, avoiding per-hook backend mixing.
- The adapter initializes ShadowHook in shared mode (mode value 0), which is designed
  to allow multiple hooks at the same target. Its API and ABI must match the host's
  loaded library.
- CI compilation does not prove runtime compatibility with the exact LeviLauncher,
  Apollon build, Android version, or Minecraft Bedrock build. Test on a disposable
  world and inspect the Runtime diagnostic `hook_backend=ShadowHook` before judging
  whether the new backend was actually selected.

## Upstream references

- ShadowHook repository: https://github.com/bytedance/android-inline-hook
- Native API manual: https://github.com/bytedance/android-inline-hook/blob/main/doc/manual.md
- Apollon repository (reference for investigation, not a guarantee of its exact runtime
  dependency): https://github.com/TheChuan1503/apollon-client-for-minecraft-pe
