# Testing2.zip and Apollon compatibility analysis

Date: 2026-10-16
Scope: isolated branch `test/apollon-current-26.52-analysis-20261016`

## Archive identity

The supplied `Testing2.zip` is an unpacked Android APK payload, not a native C++ source archive. Its entries are:

- `AndroidManifest.xml`
- `classes.dex` through `classes4.dex`
- `lib/arm64-v8a/libminecraftpe.so` and several other runtime libraries

The included `libminecraftpe.so` is an AArch64 ELF shared object, 328,419,680 bytes, marked stripped. It contains the version string `1.26.52.03_RC2`. Its dynamic symbol table has no exported `GameMode`, `Actor`, or `ClientInstance` symbols and no `getAABBDim` symbol; matches for `intersects` were unrelated text-layout methods. This means dlsym cannot validate the private engine functions from this binary alone.

The Android APK has a single `LICENSE-junit.txt`, which is not a license for Apollon's C++ implementation. The public Apollon GitHub repository at commit `dea98c031088267929b8e82de2908a0cbf7f0b9b` has no root LICENSE file. Do not transplant the project's C++ source verbatim unless the rights holder supplies clear reuse permission.

## Upstream source files confirmed

The correct paths are:

- `app/src/main/jni/main.cpp`
- `app/src/main/jni/offsets.h`
- `app/src/main/jni/hook_classes.h`

The AArch64 offsets list includes old numeric entries such as `GameMode::_attack = 0xCCC075C`, `Actor::intersects = 0xC94727C`, `Actor::getAABBDim = 0xC93AE58`, and `ClientInputCallbacks::handleDestoryOrAttackButtonPress = 0x6462B60`. They are absolute offsets relative to the mapped `libminecraftpe.so`, not signatures or stable APIs. Those offsets must not be reused merely because the supplied APK's version string is 1.26.52.03_RC2; they must be independently verified against the exact library's instruction bytes and ABI.

In `hook_classes.h`, the public wrappers call those offsets via casts such as `void (*)(GameMode*, Actor*, bool)`. In particular this does not prove the current 26.52 build has that exact entry point or calling convention. Similar caution applies to `Actor::intersects` and `getAABBDim`.

## Current experiment baseline and limits

- The existing Bedaim test branch tries exact exported ClientInstance handler names and queues the call on the ClientInstance update path.
- The supplied binary is stripped and lacks those symbols, so the current symbol-only strategy is expected to report the route unavailable on this specific library. Do not interpret queued state as a landed hit.
- The existing BedrockBounds/Hitbox Expander edits client target-query geometry only. No local AABB change can by itself alter server-authoritative collision or server reach validation.
- AutoTotem should be debugged from the existing `OffhandTransferResult` logs and inventory API validation; the APK's private native offsets do not establish a safe inventory transaction path.

## Next validation work

1. Record the APK/library SHA-256 and ELF build ID in test artifacts.
2. Compare the exact library with any IDA/Ghidra analysis for this exact build, mapping function starts and calling conventions; do not use guessed offsets or run unverified function pointers.
3. If a function signature cannot be established, keep that route disabled and rely on supported input/game APIs.
4. Build the isolated branch and collect device logs from a private controlled world; distinguish route resolution, dispatch, visible animation, health change, crash, and manual input regression.
5. Keep AutoTotem changes independent from attack-route diagnostics unless evidence establishes a shared cause.

## Validation status

This document records static archive inspection and upstream wrapper/offset inspection. It does not claim that AutoHit, AutoTotem, or true server-side hitbox expansion works in-game.

## Additional supplied APK: Apollon Client v6.7 / Bedrock 1.26.52

A second upload, `MCPE_Apollon_Client_1.26.52_v6.7_64Bit.apk`, was inspected directly.

- APK SHA-256: `7fbc51ed331295c23bf77c3349ebe145bc7719eb1caee91a33f3a75fea93e670`.
- Archive: 501 entries; approximately 357.21 MiB uncompressed.
- Package-related DEX strings include `com.subroutineZEFF.ApollonClient` and `6.7`.
- `libReverseSigma.so`: 3,432,184 bytes; SHA-256 `b132fd124e26c05656d7a9ef53769ed20923fda4c37fed712a849e557088ac74`; GNU build ID `08449f5d6527a32d499cefcc88819f32ec0dc5d5`.
- `libReverseSigma.so` exports JNI entry points for the overlay, but does not export named GameMode/Actor attack or hitbox routines. It contains the configuration strings `emulateNativeAttack`, `hitBoxValue`, and `hitBox`; these are strings/config keys, not proof that a corresponding private engine method is exported.
- Important: its `libminecraftpe.so` SHA-256 is exactly `53c4a86a8f3dbd7f0d48e440f8607d63c55100aeb364c80d19afefa8097e233a`, byte-identical to the library in `Testing2.zip`. The `libReverseSigma.so` hash is also byte-identical between the two uploads. So the new APK supplies application/package context but no newer native engine or Apollon overlay binary compared with the prior archive.

The overlay library contains native ARM64 code and can be disassembled for further static study. However, code/data references do not by themselves establish a private function's ABI, side effects, or safe call context. No old offset is promoted to a Bedaim runtime constant from this analysis alone.
