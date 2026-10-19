# 2026-10-19 — video/log review and input-binding hypothesis

## User-observed behavior

- The latest report says the bot emits particles / swing-like effects, stops looking toward the target, and deals no observable damage.
- The uploaded MP4 could not be decoded by the available runtime in this turn, so this note does not claim a frame-by-frame video review. The conclusions below are grounded in the attached diagnostics and the user's description.

## Log findings

- In the fresh session at timestamp `1792404847273`, hook installation completed, but `ClientInstance::getLocalPlayer` had no exported symbol. The validated vtable fallback at slot 32 returned null repeatedly; `resolution_failed` was emitted over and over.
- The attack observer is deliberately deferred until a live public three-argument adapter validates.
- The latest setup reported `GameMode::useItemAsAttack=false` for both GameMode and SurvivalMode, so the touchscreen repeat hook is not an available fallback in this build.
- The total log includes a prior attempt where `native_attack_accepted` occurred at 1.35 blocks with a sword, but target hurt time remained 0 before and after. Soon after, target snapshots became stale/missing and HitboxExpander reported actor-query/snapshot problems.
- A separate session reported `game_mode=present; attack_route=unavailable`; another showed the held item as `item.wind_charge.name` and correctly rejected it as an unsupported weapon profile. That rejection is expected and must not be confused with the root attack-route failure.
- The text “block” in the video description is ambiguous from the current logs: the startup line “Block-start hooks” reports hook installation, not necessarily the on-screen error itself. Do not equate the two without matching timestamp/frame evidence.

## ProtoHax research (source-backed)

- Local player and entity state are supplied by ProtoHax's host session model: `ctx.session.entityState.localPlayer` and `entities.values()`.
- Its documented `localPlayer.interactEntity(entity, true, rotation, "attack")` queues a protocol/session entity action. The API is types-only and host-resolved at runtime; it is not an exported native C++ method.
- ProtoHax's docs warn that `movement_tick` is performance-critical and runs synchronously on the game thread. Heavy scans should be deferred/cached. This reinforces keeping Bedaim's native tick path bounded.
- References:
  - https://userscript.protohax.net/api/local-player/
  - https://userscript.protohax.net/api/entities/
  - https://userscript.protohax.net/api/session/
  - https://userscript.protohax.net/api/movement/
  - https://userscript.protohax.net/guides/events-and-packets/

## Apollon research status

- The Library has a prior research report and an Apollon license text authorizing Wendell to use/modify/adapt/integrate source subject to its terms.
- The previous report describes “Simulate Native Attack” as a hypothesis, not a verified implementation detail. The APK's exact native attack call site has not been proven by the current logs, so do not copy an imagined method or treat the report as reverse-engineering evidence.
- Next APK-specific step: inspect the actual Testing2 APK/ELF or source on the connected Termux/device workspace, locate the implementation behind its native attack toggle, record ABI and call site, and compare with Bedaim's live runtime. This turn did not execute commands on the user's phone/Termux.

## Binding-mode decision

The suggested “attack key binding” mode is a valid experiment, but it must be distinct from Bedaim's module-toggle keybind. Current source's `keybind` is the launcher binding for toggling the module; it does not read Minecraft's gameplay attack control or synthesize its input. Do not just reuse that integer.

Before coding the mode:
1. Identify how this Minecraft version stores the user's attack action binding and how the game consumes it.
2. Identify a verified in-process action/input API that is safe on this Bedrock build; don't use Android OS key injection or guessed function pointers as a substitute.
3. Compare two explicit modes behind a feature flag: validated native GameMode attack vs verified gameplay-input action. Keep the mode disabled until the route is validated.
4. Preserve manual attack diagnostics separately from synthetic calls and confirm target damage in a private test world.

## Regression guardrails

- No changes to `main`.
- Do not alter Aim Assist targeting, Hitbox Expander restoration, AutoTotem, or ElytraMacing while diagnosing AutoHit unless a proven dependency requires it.
- Never call a route because a signature exists alone; validate the live receiver/vtable/ABI.
- Build success is not a runtime pass. The pass criterion is observable target damage without lag or regression.
