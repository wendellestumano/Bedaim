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


## Binary-level validation of Apollon offsets

The 1.26.52 v6.7 APK was extracted and inspected with ELF section metadata and AArch64 disassembly:

- The named `.text` section starts at virtual address `0x64C64E0`. The `.eh_frame` section spans `0x452C1D0` to `0x64C64DC`.
- Apollon's old `ClientInstance::handleDestoryOrAttackButtonPress` offset `0x6462B60` lies inside `.eh_frame` unwind metadata, not inside the named `.text` code section. It is not a valid code entry for this exact library. No numeric fallback to it is enabled.
- At `0xCCC075C`, the current binary disassembles to `str x8, [x0]` inside an allocation/object-initialization instruction sequence, not the start of a GameMode attack routine.
- At `0xC94727C`, the current binary contains a conditional branch within cleanup/free logic; at `0xC93AE58`, it contains a stack-address calculation inside another routine. These locations do not validate the old `Actor::intersects` or `Actor::getAABBDim` function entries.

These observations are specific to the exact supplied `libminecraftpe.so` SHA-256 recorded above. They reject those old offsets for this binary; they do not claim the corresponding concepts/functions do not exist at different addresses.

## Behavior observed in the public Apollon source

The inspected `app/src/main/jni/main.cpp` and `hook_classes.h` indicate:

- In the player KillAura path, `emulateNativeAttack` prevents the direct `myGameMode->attack(target, true)` call, marks a pending attack request, and invokes the ClientInstance attack-button handler from the ClientInstance tick hook. The pending flag is then cleared.
- That behavior is an input-handler route, not a call to the private `GameMode::_attack` implementation. The source also calls GameMode directly in other paths, including the examined MobAura branches; the option is not a universal replacement for every attack.
- The examined code expands client-facing target dimensions by changing the returned AABB dimensions used for local selection. This does not demonstrate server-authoritative collision expansion.
- No AutoTotem implementation was found in the examined public `main.cpp`/configuration paths. Bedaim AutoTotem remains an independent inventory-transaction investigation.

## Bedaim experimental implementation added

On branch `test/apollon-current-26.52-analysis-20261016`:

- Added an independent, symbol-only attempt to resolve exported ClientInstance attack-button handler spellings and queue one request for the ClientInstance update path.
- Explicitly prohibited reuse of Apollon's numeric offsets.
- Replaced the experimental direct internal attack-trampoline call with the verified public GameMode wrapper fallback. The internal function body bypasses setup performed by the public wrapper and must not be called directly.
- Updated AutoHit diagnostics so that accepting or queueing a request is not reported as proof of damage.

The supplied engine is stripped and the relevant handler symbols are not exported, so the symbol-only handler route is expected to report unavailable on this exact APK. In that case the branch tries only the exact-signature public GameMode wrapper route. If neither validates, AutoHit remains blocked safely and needs further version-specific analysis.

## Validation caveats

CI can verify host regressions, Android ARM64 compilation, and experimental package identity. It cannot verify the runtime ABI, whether a target was damaged, AutoTotem transfer success, or server collision. Those still require the exact device/game build and captured runtime logs.


## Signature-pattern scan against the exact engine binary

The current Bedaim signature patterns from `src/core/memory/Signatures.cpp` were scanned against the exact `libminecraftpe.so` in this APK:

- `GameModeAttack` pattern: one match at virtual/file offset `0xF89FA74`. AArch64 disassembly there is `mov x3, x2; mov w2, #1; b 0xF89FA80`, followed by a native function prologue at the branch target. This shape is consistent with a public three-argument attack wrapper preparing the fourth argument and defaulting the predictive-sound flag.
- A relative relocation at `0x13016E18` points to `0xF89FA74`; it sits at the 16th 8-byte entry of a vtable-like function-pointer cluster. That supports the public wrapper hypothesis, but the runtime code must still verify that the active object's vtable slot 16 is exactly the resolved pattern address before calling it.
- `GameModeAttackInternal`, `SurvivalModeAttack`, and `ClientInstanceUpdate` patterns from the current signature table had zero matches in the exact file. Thus the experimental direct-internal route cannot be considered validated here, and the ClientInstance update hook signature used by the handler adapter is not established by this scan.
- `ActorGetOffhandSlot` matched twice, so it is ambiguous without additional runtime/object validation. The inventory transaction must remain fail-closed if its getter, setter, transaction manager or action constructors cannot be validated.

This evidence explains the new route priority: never call the unmatched internal attack body; only attempt the public wrapper after its signature and the live GameMode vtable entry agree. The symbol-only native input route remains unavailable for this stripped binary unless an exact exported handler is found and the required ClientInstance dispatch context is established.
