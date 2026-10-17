# Changelog

## 2026-10-17 — BedrockAim, BedrockBounds and ShiftBlade integration

- Integrated the BedrockAim target-tracking updates and BedrockBounds native AABB expansion on a branch based directly on `main`, without merging the divergent experimental history.
- Added the **Ver Hitbox** preview and connected it to the existing hitbox renderer.
- Reduced Hitbox Expander AABB oscillation by keeping active expanded bounds stable between ticks, translating saved native bounds with actor movement, pruning stale/out-of-range targets, and restoring bounds when disabled or when target-type settings change.
- Added **ShiftBlade** with configurable target range and allowed height above stable ground. It now keeps a sword equipped while the target remains in range by default, yields to consumables and Mace Swapper priority, and offers **Respect manual hotbar changes** for users who prefer manual selection.
- Fixed a ForceGlobalRP packet-hook guard that checked the ResourcePacksInfo trampoline before calling the ResourcePackStack trampoline.
- Registered the existing **AutoReQ** module, which previously had source files but no registry entry.
- Kept AutoHit and AutoTotem out of this integration.
- Host regression tests and an Android ARM64 package build must pass on the final branch head before merging to `main`; in-game behavior still requires runtime testing.
