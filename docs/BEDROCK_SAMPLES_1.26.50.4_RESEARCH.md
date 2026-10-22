# Bedrock Samples v1.26.50.4 — Module audit

## Inputs examined

- bedrock-samples-v1.26.50.4-full.zip: 22,859 entries, including behavior-pack item/entity definitions and resource-pack UI/assets.
- diff-v1.26.40.05_to_v1.26.50.4.zip: 2,449 entries; its _diff.txt reports 712 added, 1,736 changed, and 1,426 removed paths between the named sample revisions.

These are Add-On/resource-pack samples, not Minecraft native C++ source or ABI headers. They are useful for item identifiers, declared item components, and UI/resource structure; they cannot validate native offsets, vtable slots, or internal attack methods.

## Changes applied

### Aim Assist: spear prediction

The seven vanilla spear definitions in the full sample are wooden, copper, golden, stone, iron, diamond, and netherite. For example, behavior_pack/items/wooden_spear.json defines minecraft:is_spear, minecraft:piercing_weapon with reach min 2.0 / max 4.5, and minecraft:kinetic_weapon; it does not define minecraft:projectile. The related files use weapon cooldown/swing-duration metadata. This supports treating Spears as melee/kinetic weapons in the Ranged Aim Assist prediction setting rather than applying projectile lead to them.

Aim Assist already recognizes identifiers containing spear; no guessed item IDs or offsets were added. Regression tests keep Spears and other melee categories out of ranged-only prediction while preserving Bow/Crossbow/Trident behavior.

The policy extraction also exposed an enum-boundary bug: cached Tool (the last HeldWeapon value) was clamped to OtherMelee. This is corrected so tools no longer inherit the other-melee aim point/speed profile.

## Candidates deliberately not copied

- The version diff modifies vanilla resource_pack/ui/hud_screen.json and UI globals, but replacing Bedrock full HUD layout would be risky for Bedaim native overlays and existing resource packs. No vanilla UI files were copied.
- Sample player collision dimensions (0.6 wide / 1.8 high) are entity-pack defaults, not a reliable substitute for runtime native bounds; Aim Assist and Hitbox Expander should continue using live bounds where available.
- Sample item reach values are server/gameplay component data. Aim Assist configured scan range is a target search distance, not proof of effective client attack reach, so those values were not copied into native reach/attack calls.
- The samples do not validate signatures/offsets of native Player, GameMode, attack, gliding, or inventory functions. Those remain runtime-specific and require separate verification.

## Validation

- Host regression coverage is added for the ranged-prediction weapon policy.
- Android ARM64 build and levipack packaging must pass CI before considering this sample-driven change build-verified.
- In-game confirmation is still required for aim response across spear variants and all runtime-specific modules.
