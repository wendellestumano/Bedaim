# Changelog

## 2026-10-17 — BedrockAim, BedrockBounds and ShiftBlade integration

- Integrated the tested BedrockAim tracking improvements and BedrockBounds client-side AABB expansion into a mainline-based integration branch.
- Fixed moving target bounds by retaining a stable unexpanded AABB baseline and translating it with actor movement each tick.
- Added the **Ver Hitbox** preview setting and connected it to the existing hitbox renderer.
- Added **ShiftBlade**, which selects a sword when the BedrockAim target is in range and the player is grounded by default; the new **Allowed distance above ground (blocks)** setting permits a configurable small height above the last stable ground level.
- ShiftBlade yields to consumables, the Mace Swapper's slam priority, and explicit manual hotbar changes.
- Retired AutoHit and AutoTotem controls/behavior from this integration and removed their related tests.
- Existing modules remain registered; Android build and host tests are required before merging to main.
