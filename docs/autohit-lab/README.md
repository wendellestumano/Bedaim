# AutoHit lab notes

This directory is the project's durable experiment log. Update it whenever a build is tested or a hypothesis is ruled out. Do not mark a fix as successful from compilation alone.

## Rules

- Work on an isolated branch; never make experimental AutoHit changes directly on `main`.
- Record branch, commit SHA, build/run number, artifact, device/runtime, test setup, observed output, and next hypothesis.
- Separate compile/test success from in-game attack success.
- A native wrapper returning `true`, particles, swing animation, or an internal Attack event is not proof of server-accepted damage.
- Keep manual attack and synthetic AutoHit attempts distinguishable in logs.
- Avoid unchecked vtable slots, guessed native ABI, or unvalidated function pointers.
- Avoid expensive target scans and signature/vtable resolution in the per-tick hot path.

## Current baseline (2026-10-19)

- Branch: `fix/autohit-input-binding-memory-20261019`
- Parent commit: `4e35f1ea6052fabf3d2dae1d335016850821ecc0` (Build #844, ARM64 package succeeded; this is not runtime proof).
- Latest user report: AutoHit still emits particles / swing-like feedback, stops tracking the target, and causes no observed damage.
- Relevant log evidence:
  - `ClientInstance::getLocalPlayer` exact export is missing; fallback vtable getter at slot 32 repeatedly returns null.
  - The public `GameMode::attack` route is not validated in the live vtable. One logged resolver state had `public_sig=0x0`, `survival_sig=0x0`, and only an internal signature candidate.
  - Earlier runs reported `native_attack_accepted` with target hurt time 0 before and after. That is not damage confirmation.
  - Target snapshots later become stale/missing; HitboxExpander also logged `active_actor_query_failed` and `actor_snapshot=malformed`.
  - `GameMode::useItemAsAttack` hooks were not installed in the latest baseline. Do not treat them as a working fallback.
- Conclusion: retry throttling only contains lag. It does not fix the attack route or local-player/target lifecycle. The failure can occur before dispatch, and apparent successful dispatch has not been shown to produce damage.

## Input-binding proposal

The proposed binding mode means: read the user's Minecraft attack binding and trigger the game's own input/action path only while a validated target is in reach. It is distinct from Bedaim's module-toggle keybind.

Do not implement by synthesizing arbitrary OS key events or by calling a guessed input handler. First identify a supported in-process input binding accessor and the matching action dispatch path for this exact Bedrock/Levi runtime. If the native input path cannot be verified, keep the option unavailable rather than pretending it works.

ProtoHax UserScript documents `localPlayer.interactEntity(entity, true, rotation, "attack")`, but that runs in ProtoHax's host-provided session/protocol runtime and its tracked network entities. It is not a native Bedrock C++ ABI that can be copied directly into Bedaim.

## Next experiments (ordered)

1. Fix local-player resolution and prove the resolved pointer is stable and matches the actual local actor. Do not use a null result as a valid player tick.
2. Add a single compact route diagnostic that distinguishes: no GameMode, public route absent, internal signature only, adapter rejected, dispatch rejected, and dispatch accepted without authoritative damage.
3. Inspect the exact BedrockTools/Levi SDK headers and symbols for the current version before changing any vtable assumptions. Do not assume slot 32 or slot 16 from another build is portable.
4. Determine whether the current camera/targeting module invalidates its own target snapshot when the local-player getter returns null; fix lifecycle coupling before another target scan optimization.
5. Prototype input binding behind a separate disabled-by-default option only after an actual input binding/action API is found. Keep current native route as a separate mode for A/B testing.
6. Test on a private world with a stationary target and record manual attack baseline vs AutoHit. Confirm damage through target health/hurt events or server-observable state, not particles or client-side wrapper return.
7. Run host regression tests and ARM64 package build; then perform in-game test and attach logs/video. Update this file with the result.

## Build history

| Build / commit | Result | Interpretation |
|---|---|---|
| #837 | Compiled; runtime test failed | AutoHit did not damage, lag and particle/render symptoms remained. |
| #841 / `30329b0577242a13b21027f8cc6da55d8546de3b` | Build succeeded | Retry throttling only; no runtime success established. |
| #844 / `4e35f1ea6052fabf3d2dae1d335016850821ecc0` | Host regression tests and ARM64 package succeeded; artifact `BedrockTools-levipack` exists | Backoff raised to 5 s; this is a performance containment change, not an AutoHit fix. |
