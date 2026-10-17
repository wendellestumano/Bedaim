# Bedrock Add-On and LeviLamina research for Bedaim

Date: 2026-10-17  
Scope: research notes only; no external add-on code copied into the native module.

## Executive finding

The official Bedrock Script API contains a ray-cast API, `Entity.getEntitiesFromViewDirection(options)`, which returns an array of hit records with `entity` and `distance`. The associated `EntityRaycastOptions` supports a maximum distance, block collision behavior, liquids, and passable blocks. This is a useful behavioral reference for target-ray validation and tests. It is a JavaScript/TypeScript Script API, not a drop-in C++ function, and must not be called from Bedaim by guessing an address.

Official references:
- Script API Entity: https://learn.microsoft.com/en-us/minecraft/creator/scriptapi/minecraft/server/entity
- EntityRaycastOptions: https://learn.microsoft.com/en-us/minecraft/creator/scriptapi/minecraft/server/entityraycastoptions
- EntityRaycastHit: https://learn.microsoft.com/en-us/minecraft/creator/scriptapi/minecraft/server/entityraycasthit

## Repositories inspected

1. Mojang Bedrock Samples — https://github.com/Mojang/bedrock-samples  
   Official repository describes itself as current behavior/resource Add-On samples. Useful for entity definitions, component groups, animations, render controllers, particles, textures and resource/behavior JSON. It does not provide native `GameMode::attack` or an offhand native setter.

2. Microsoft Minecraft Samples — https://github.com/microsoft/minecraft-samples  
   The README lists starter Add-On, behavior pack, custom items/sounds, debug tools, and complete creator samples. Useful for resource/behavior pack design and visual testing patterns, not for reverse-engineering the native C++ client.

3. Microsoft Minecraft Scripting Samples — https://github.com/microsoft/minecraft-scripting-samples  
   Contains TypeScript `@minecraft/server` samples. These demonstrate event-driven inventory and gameplay logic, but are Script API samples and may require a world/add-on capability and module version not present in the native Levi launcher context.

4. LeviLamina — https://github.com/LiteLDev/LeviLamina  
   This is the more relevant native-C++ research lead. Its `ScriptActor.h` wrapper exposes Script API concepts such as AABB lookup, view-direction block raycasts, and entity view-direction raycasts. It is generated against a particular Bedrock/LeviLamina version and uses its script-runtime types, lifetime handles, and engine bindings. Reusing the method signature alone is unsafe; first establish that Bedaim's exact runtime includes compatible headers/libraries and that the wrapper is usable in the Android client/Levi version targeted.

Relevant source browsed:
- LeviLamina ScriptActor header at commit `917f0defa15ffb34599eb4522da1617a4a55b42b`: https://github.com/LiteLDev/LeviLamina/blob/917f0defa15ffb34599eb4522da1617a4a55b42b/src/mc/scripting/modules/minecraft/actor/ScriptActor.h
- LeviLamina README / C++ mod-loader description and LGPL-3.0 notice: https://github.com/LiteLDev/LeviLamina/blob/main/README.md

## How this applies to current Bedaim code

### Target selection and crosshair confirmation
Bedaim already computes target bounds, selects aim points, and calls `AimAssistModule::currentTargetInCrosshair`. It also validates that target data is recent. Compare this geometry against the official raycast mental model:
- Start from the actual eye/view origin, not feet or body center.
- Normalize view direction and validate all vector components.
- Calculate nearest non-negative ray intersection with an AABB, not only angle/distance to its center.
- Honor block occlusion when the setting requires line of sight.
- Prefer the nearest valid intersection along the ray; don't let a farther box win because its center is closer to a chosen anchor.
- Use the result for observation/diagnostics and regression tests before changing native attack dispatch.
The script API does not establish whether a native expanded AABB is respected by Bedrock's raycast, so that must be checked in the native runtime.

### Native player identity / tick regression
The current logs prove the local-player signature lookup is unavailable. An API wrapper from ScriptActor is not itself a solution for finding the client-side local player: it expects a live Actor and scripting lifetime context. Don't substitute a script actor, server player list, guessed vtable index, or an arbitrary `NormalTick` receiver for local-player identity.
The next useful research target is LeviLamina's version-matched C++ client/runtime interfaces and exported symbols used to reach `ClientInstance`/LocalPlayer in the current Bedrock client build. Any candidate source needs to be cross-checked against the actual runtime version and pointer ownership/lifetime before feeding tick events.

### AutoHit / attack
Current diagnostics reported the public attack signature but not a validated active GameMode attack route, and both `UseItemAsAttack` hooks failed. Official add-on raycasting can inspire a safe target-intersection test, but it cannot validate Bedaim's native attack function or its ABI. Keep the existing native dispatch guard, target freshness check, range gate, and per-weapon CPS cap. Do not create an alternate function pointer from the Script API declaration.

### AutoTotem / inventory
Official Script API samples read an inventory component/container and verify component/container presence; some examples use `Container.moveItem`/`transferItem`. These are high-level managed objects with engine validation and are not equivalent to the native Player offhand slot or transaction manager. Use them as a design reference for preconditions and postcondition verification only. Do not copy those APIs into native inventory code or infer a native vtable index from their behavior.

## Suggested controlled experiments

1. Add pure C++ unit tests for ray-vs-AABB intersection: center hit, edge hit, miss by a small angle, target behind eye, inverted/NaN bounds, nearest-of-two targets, and block occlusion policy.
2. Expose diagnostic-only values: eye origin, view direction, AABB entry distance, target age, occlusion result, and chosen target ID. Keep logs rate-limited.
3. Explore a separate research-only integration prototype for LeviLamina's C++ scripting wrapper. Compile it only if the correct version-matched headers and libraries already exist in the target toolchain; do not add a new mandatory dependency to the shipping build yet.
4. For local-player recovery, record available exported symbols from the loaded client/Levi images and verify signatures on the exact Bedrock build. Require repeated non-null reads plus plausible actor invariants and world identity before treating a candidate as local; this validation should be version-aware and must fail closed.
5. Test the actual Bedrock/Levi Android runtime after CI passes. Host tests and an ARM64 package build do not prove symbol resolution, native ABI compatibility, or gameplay behavior.

## Compatibility and licensing caution

Official Add-On source and Script API examples target managed scripts, behavior packs, and resource packs. They can improve experimental packs and can serve as a reference for algorithms, but they are not the same API surface as Bedaim's C++ client module. Review the specific source-file license before copying any third-party code; prefer documenting the behavioral pattern and implementing it independently. LeviLamina states that its non-closed source portions use LGPL-3.0; any direct code reuse must comply with the relevant license and compatibility requirements.

## Additional local-player source lead from LeviLamina headers

At LeviLamina commit `917f0defa15ffb34599eb4522da1617a4a55b42b`, the client headers declare:
- `src-client/mc/client/game/IClientInstance.h`: abstract `virtual LocalPlayer* getLocalPlayer() const = 0;`
- `src-client/mc/client/game/ClientInstance.h`: `virtual LocalPlayer* getLocalPlayer() const override;`

References:
- https://github.com/LiteLDev/LeviLamina/blob/917f0defa15ffb34599eb4522da1617a4a55b42b/src-client/mc/client/game/IClientInstance.h
- https://github.com/LiteLDev/LeviLamina/blob/917f0defa15ffb34599eb4522da1617a4a55b42b/src-client/mc/client/game/ClientInstance.h

This is evidence that a versioned native client interface exists in LeviLamina's source tree, not proof that its exact vtable index, layout, header package, or ABI matches Bedaim's target client. Follow-up should establish the precise Bedrock build and LeviLamina/client SDK version, then compare symbol/signature routes and interface slot only against version-matched declarations. Do not substitute an index into Bedaim from this header without ABI verification.


## Status

- Research/documentation only.
- Build #619 completed successfully with host regression tests and Android ARM64 package creation.
- Local-player tick recovery, in-game target-ray behavior, and native AutoHit/AutoTotem runtime behavior are still unverified.
