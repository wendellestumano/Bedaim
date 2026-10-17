# Local-player tick recovery — 2026-10-17

## Regression
The diagnostic branch strictly publishes `LocalPlayerPreTickEvent` and `LocalPlayerTickEvent` only when the actor passed to `NormalTick` equals the pointer returned by `ClientInstance::localPlayer()`. The `ClientInstanceGetLocalPlayer` signature is unresolved on the target build, so this guard suppressed the tick events required by Aim Assist, Hitbox Expander, AutoHit, and AutoTotem.

## Recovery implemented
- Keep the strict identity comparison; do not treat an arbitrary `NormalTick` actor as the local player.
- First use the existing signature-based getter when it resolves.
- If it does not resolve or returns null, use the SDK-declared `offsets::VTable::ClientInstanceGetLocalPlayer` slot as a fallback. The fallback checks that the client object and vtable range are readable, that the selected method belongs to `libminecraftpe.so`, and that the returned object's vtable is readable and its first function belongs to `libminecraftpe.so`.
- Emit distinct diagnostics: `ready`, `ready_vtable_fallback`, `getter_signature_unresolved_fallback_failed`, and `getter_returned_null_fallback_failed`.
- If validation fails, keep events suppressed. Do not guess another slot or use the tick actor as fallback.

## Why this is still a candidate
A mapped pointer and code-image check do not prove that a method at a version-specific slot has the expected semantic signature. The slot is an existing SDK constant, not a newly guessed index, but it must still be confirmed on the exact Minecraft Bedrock/Levi runtime. CI and live logs are mandatory before merging.

## Test checklist
1. Host regression tests and Android ARM64 build pass.
2. Module Diagnostics exports logs automatically to `/sdcard/Download/Bedaim`.
3. In game, `GameHooks/local_player_resolution` reaches `ready` or `ready_vtable_fallback`.
4. `GameHooks/local_player_tick` no longer reports `local_player_unavailable` during active gameplay.
5. Aim Assist and Hitbox Expander tick normally.
6. AutoHit validates its attack route; AutoTotem validates the offhand/source and transfer postconditions.
