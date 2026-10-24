# Apollon-to-Bedaim integration — 2026-10-24

## Scope

This branch ports compatible algorithms and defensive rules from the analysed Apollon source into Bedaim's own module and SDK architecture. It deliberately does not transplant raw native offsets or binary-only helpers whose exact calling convention and object argument have not been verified for the target game build.

## ESP locator

Apollon's public ESP path obtains a view matrix, a camera position and an FOV pair, then performs a camera-space projection. Bedaim does not currently expose a verified equivalent view-matrix accessor in its SDK. This branch therefore applies the same projection *principle* with Bedrock's known pitch/yaw camera basis and an explicit vertical-FOV parameter; the runtime fallback remains 110 degrees. The code specifically checks camera-space depth, so a target behind the camera cannot be mistaken for an on-screen target merely because a wrapped yaw angle is small.

The locator now keeps a bounded, distance-sorted snapshot of up to 24 different off-screen players. Each player gets one arrow. When several arrows intersect the same screen edge, alternating offsets reduce overlap. The HUD receives copied world coordinates and only an opaque numeric identity token; it never dereferences the actor pointer from the renderer callback. The `TessellatorVertex` signature has also been updated against the supplied engine: the previous pattern had zero matches, while the new 44-byte ARM64 prefix matches the vertex(x,y,z) entry once at RVA `0xA76EFF4`. The `Actor::getNameTag` signature has also been tightened to the exact 48-byte entry prefix (one match at RVA `0xF5835B4`); the former pattern matched three unrelated string-returning functions. Its return type is now `std::string` by value, matching the observed AArch64 hidden-result (`x8`) convention instead of a reference return.

## Game-mode classification and memory guards

Apollon's analysed native classifier reads `ActorGameTypeComponent` values `1=Creative`, `5=Default`, and `6=Spectator`, with an actor property fallback called `gameType` when the component value is `Default`. Those values differ from Bedaim's normalized `Player` game type values (`0=Survival`, `1=Creative`, `2=Adventure`, `3=Spectator`). The code must keep those representations separate. The raw Apollon helper RVA and old `isAttackableGamemode` offsets are intentionally not used. Instead, Bedaim resolves the component getter with a 96-byte ARM64 signature verified to have exactly one match in the supplied `libminecraftpe.so` (file offset/RVA `0xF9BA104`); static disassembly of Apollon's call site shows the first argument is `Player + 0x8`, the `EntityContext`. If that build-specific signature does not resolve, the code fails closed. The getter is used only for unambiguous Creative and Spectator mapping; `Default=5` stays unknown unless Bedaim's own Player API or observed setter cache provides the real mode. The legacy attackability predicate remains binary and is not an exact-mode detector.

Before native per-actor calls, the updated GameHooks provides short-lived `/proc/self/maps` checks for readable and executable mappings, validates the object's first vtable entry, and ensures function targets reside in executable mappings. The new component getter is signature-resolved and called with the confirmed `EntityContext` argument. It is integrated as a strict fallback into both GameMode Checker/Aim Assist mode lookup. GameMode Checker checks the full used actor-vector range, copies entries using `memcpy`, checks each actor's vtable before classifying it, and only calls the version-resolved name-tag getter when its target is executable.

These checks reduce common invalid-pointer failures but are not a proof of object lifetime or memory safety: mappings may change between validation and use, and a valid executable function may still reject an invalid object. `try/catch` is not assumed to recover from native SIGSEGV/SIGBUS. Actual crash resolution still requires testing in the target Minecraft process and checking the new diagnostics/tombstone if a crash persists.

## Tests and remaining verification

`tests/launcher/esp_locator_math_test.cpp` now checks that (1) targets behind the camera are off-screen and (2) the result changes appropriately with the vertical FOV parameter. The Android NDK package build and in-game verification have not been represented as complete until they actually pass.
