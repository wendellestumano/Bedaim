# Bedaim recovery notes — 2026-10-17

## Evidence from the supplied runtime log

- Module registration succeeds (56 modules initialized).
- Aim Assist and Hitbox Expander both report native dependency probes as ready, but they depend on `LocalPlayerPreTickEvent` / `LocalPlayerTickEvent`.
- The current-player resolver reports that `ClientInstance::getLocalPlayer` resolves to a function but returns null. The strict tick filter therefore suppresses those events.
- Attack route diagnostic: public attack signature is present, but the Survival/internal attack signatures and internal trampoline are absent. This does not itself prove the public wrapper is unusable; it must pass the runtime vtable/symbol match used by `attackAvailable`.
- The offhand getter pattern is found, but its returned `ItemStack` pointer must pass runtime validation. The submitted log ends before AutoTotem records any offhand read or transfer attempt.

## Recovery changes

1. Reuse the Aim Assist / Hitbox Expander implementation from Build #469 (`integrate/bedaim-shiftblade-hitbox-20261017`), which passed host regression tests and an Android ARM64 package build.
2. Keep the identity filter. When the pattern-based getter fails, try the SDK's documented `ClientInstanceGetLocalPlayer` vtable slot (index 32, adjacent to `getRegion` at index 31) only after validating the slot pointer belongs to `libminecraftpe.so` and validating the returned player's vtable.
3. Preserve Module Diagnostics and its configured periodic rolling log export.
4. AutoHit still requires a fresh Aim Assist target, centered/valid target bounds, valid player/GameMode identity, and a public attack wrapper whose live vtable entry matches a known resolved signature or exact exported symbol.
5. AutoTotem still refuses to move an item unless the offhand read is valid, the offhand is empty, the totem source slot is valid, the transaction-manager state passes validation, and the required native transaction/setter APIs are present. It must not overwrite a shield or other offhand item.

## References checked

- LeviLamina `ClientInstance.h` declares `getRegion()` followed by `getLocalPlayer() const`.
- LeviLamina `IClientInstance.h` exposes the same getter contract.
- Microsoft Bedrock Script API has `Entity.getEntitiesFromViewDirection` and managed `Container.moveItem/transferItem`; these inform behavior-level raycast/inventory expectations but are not native C++ symbols and must not be cast into Bedaim's ABI.
- Native symbol names, vtable positions, and object layouts must match the target Bedrock version. Current-source reference alone is not a runtime proof.

## Validation status

- Build #469: host regression tests and Android ARM64 package creation succeeded for the stable Aim Assist / Hitbox Expander base.
- Build #623: the diagnostic/local-player branch compiled successfully before this recovery.
- The recovery commit itself is not validated until its own CI run completes.
- Even a successful CI build cannot prove in-game hit tracking, public attack dispatch, or offhand transfer; runtime logs must show the corresponding `ready` and postcondition states.
