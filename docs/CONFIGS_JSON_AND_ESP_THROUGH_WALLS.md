# Configs.json and ESP through-wall rendering

- The active config file is now `Configs.json` in the same launcher-provided config directory as the previous `config.json`.
- `ConfigManager` already serializes settings for every registered module under the top-level `Modules` object; the new filename keeps that complete snapshot.
- On first launch after this change, if `Configs.json` does not exist and the old sibling `config.json` does, Bedaim loads the old file and writes a migrated `Configs.json`. It leaves the old file untouched as a backup.
- Normal player ESP boxes and its two target-ring layers now use the same native no-depth RenderDragon material path as Ore ESP. Native Hitbox/expanded-hitbox drawing remains depth-tested.
