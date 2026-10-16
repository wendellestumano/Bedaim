# Apollon test branch notes

This branch is an isolated experiment and is not merged into `main` or the normal Bedaim PR.

## Input package analyzed

- Uploaded archive: `Apollon_code.zip` (APK-style ZIP containing DEX files and ARM64 native libraries).
- `libminecraftpe.so` includes the Bedrock version string `1.26.52.03_RC2`.
- `libminecraftpe.so` GNU Build ID: `56de9eed077631e03a31f4f58eb2f0e00071d335`.
- `libReverseSigma.so` GNU Build ID: `08449f5d6527a32d499cefcc88819f32ec0dc5d5`.
- The native library contains the string `emulateNativeAttack`.
- The public Apollon source shows that when `emulateNativeAttack` is enabled, it queues `ClientInstance::handleDestoryOrAttackButtonPress()` from the client update path. The public source also exposes a direct `GameMode::attack` wrapper.

## Source reference

- Public source inspected: https://github.com/TheChuan1503/apollon-client-for-minecraft-pe
- This is a compatibility experiment based on the supplied archive and public source behavior; the normal Bedaim branch and `main` remain unchanged.

## What this test build changes

- The normal build continues to use the validated public GameMode attack route.
- This branch uses a test-only direct internal attack route, calling the `GameModeAttackInternal` trampoline discovered by the current Bedaim signature resolver instead of the public vtable wrapper.
- The experimental route requires the current signature resolver to find `GameModeAttackInternal` and the hook trampoline to be installed; otherwise it refuses to dispatch.
- No historical Apollon offsets are copied. The public source's offsets are version-specific and do not establish valid addresses for Bedrock 1.26.52.03_RC2.
- This tests the direct internal attack path inspired by Apollon's `GameMode::_attack` call. It is **not yet a full port of Apollon's native input-handler route**; the supplied current-build binary does not expose a usable symbol for `ClientInstance::handleDestoryOrAttackButtonPress`, so a compatible signature/hook must be independently validated before calling it. The current test build intentionally tests the internal attack trampoline instead.

## Testing checklist

1. Install only the `BedrockTools-ApollonTest-26.52` artifact from this branch, not the normal package.
2. Back up worlds first and start in a private test world.
3. Confirm the game opens without crashing.
4. Enable Aim Assist and AutoHit; test a stationary mob first, then a moving mob.
5. Save the Bedaim log immediately after each attempt.
6. Report whether the game crashed, whether the swing animation played, whether the target took damage, and the first lines mentioning `Apollon-style direct internal attack route`, `AutoHit`, or `GameModeAttackInternal`.

Build result for the initial experiment: host regression tests and Android ARM64 packaging passed in workflow run #1. A successful compile only proves that the experiment builds; it does not establish that the native ABI or hit registration is correct in-game.
