# BedrockTools upstream compatibility audit — 2026-10-24

## Reference source

- Public source repository: https://github.com/QYCottage/BedrockTools
- Current upstream checkout identifies itself as BedrockTools **1.4.9** in `include/bedrocktools/Version.hpp`.
- The supplied `BedrockTools.levipack` is reported as **1.5.7**, so this source tree is a useful architectural reference, but it is **not proven to be the exact source** used to build that binary.

## Safe compatibility correction

Restored `FrameEvent` to the upstream observational contract. It is published from the `eglSwapBuffers` hook to notify HUD/render modules; the event should not be cancellable. The real EGL swap must always reach its original implementation.

## Deliberately preserved

This change does not overwrite Bedaim's shared runtime wholesale. In particular, it preserves Bedaim-specific:

- `AttackResult` event and combat-event extensions;
- Aim Assist, Hitbox Expander, ESP and other added modules;
- diagnostic logging and native symbol probes;
- config serialization safeguards and launcher configuration extensions;
- custom game hooks required by existing modules.

Several of these files differ substantially from upstream and contain Bedaim-only behavior. Blindly copying upstream files over them could remove module registrations or break event/hook contracts.

## Validation still required

- Host regression tests and Android ARM64 packaging;
- in-game launch and world-entry test with the updated package;
- runtime logs for any remaining native crash.

This audit is not a claim that all regressions are fixed. Additional repairs should be isolated by subsystem and checked against upstream behavior before changing shared native hooks or SDK layouts.
