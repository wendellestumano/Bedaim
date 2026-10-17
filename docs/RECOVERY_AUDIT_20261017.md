# Bedaim recovery audit — 2026-10-17

## Goal
Recover Aim Assist and Hitbox Expander from the last green integration snapshot while preserving Module Diagnostics, including automatic rolling log export to `/sdcard/Download/Bedaim`, and retaining AutoHit/AutoTotem for focused repair.

## Findings grounded in the repository
- Last known green integration: commit `78449d76c04e488475b0ecb108bd8d7fb3659fea`, Build #469: https://github.com/wendellestumano/Bedaim/actions/runs/39001837405
- Experimental combat branch base: `77254f0bcc952c4c5cc6c32c508cdd72acddc868`, Build #589: https://github.com/wendellestumano/Bedaim/actions/runs/39030960327
- Previous diagnostics identified an unresolved local-player getter that suppressed player tick events. The recovery candidate restores the known-green `NormalTick` event path rather than weakening an identity check by accepting an arbitrary actor.
- AutoHit requires a fresh Aim Assist target snapshot, crosshair/range validation, and a validated `gameModeForPlayer` / `attackAvailable` route.
- AutoTotem requires a readable offhand, valid source slot, verified native transfer API, and post-transfer verification. Managed Add-On APIs are behavioral references, not native client APIs.

## External research
- LeviLamina declares `IClientInstance::getLocalPlayer() const` and `ClientInstance::getLocalPlayer() const`. This confirms an API surface, not the target runtime ABI/signature or vtable slot; do not transplant indices blindly.
  - https://github.com/LiteLDev/LeviLamina/blob/main/src-client/mc/client/game/IClientInstance.h
  - https://github.com/LiteLDev/LeviLamina/blob/main/src-client/mc/client/game/ClientInstance.h
- Microsoft documents `Entity.getEntitiesFromViewDirection` with hit entity and distance for Add-On raycasts. It can inform target-selection tests, but `@minecraft/server` objects do not replace native C++ calls.
  - https://learn.microsoft.com/en-us/minecraft/creator/scriptapi/minecraft/server/entity?view=minecraft-bedrock-stable
  - https://learn.microsoft.com/en-us/minecraft/creator/scriptapi/minecraft/server/entityraycasthit?view=minecraft-bedrock-stable
- Microsoft documents `EntityEquippableComponent.getEquipmentSlot` and `setEquipment` for the offhand in behavior-pack scripts; this does not validate Bedaim's native inventory transaction layout.
  - https://github.com/MicrosoftDocs/minecraft-creator/blob/main/creator/ScriptAPI/minecraft/server/EntityEquippableComponent.md
- LeviLamina's v26.51.7 release is dated 2026-10-12 and notes runtime-data and PreLoader changes. The target Minecraft/Levi version must match before relying on its current headers.
  - https://github.com/LiteLDev/LeviLamina/releases

## Validation checklist
1. Host tests pass: AimAssistMath, AutoHit guards/weapon profile, AutoTotem policy, diagnostics, and atomic config export.
2. Android ARM64 package builds successfully.
3. Module Diagnostics confirms tick-hook installation and local-player tick delivery.
4. Aim Assist acquires fresh targets without orbiting/stuttering; Hitbox Expander restores saved bounds when disabled and when actors despawn.
5. AutoHit reaches a validated attack route; a dispatched call is not treated as confirmed damage unless the native result is observed.
6. AutoTotem logs readable offhand/source state, transfer result, and verified postconditions. No transfer is attempted when prerequisites are unresolved.

A green CI build is not proof of runtime correctness. Keep this branch isolated until the in-game checklist passes.
