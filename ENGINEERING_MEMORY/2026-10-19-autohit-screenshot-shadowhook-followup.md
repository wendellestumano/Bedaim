# AutoHit follow-up — screenshot warnings and ShadowHook research (2026-10-19)

## Screenshot evidence

The uploaded Module Diagnostics panel shows: `AutoTotem: tick_gate=disabled`; `HitboxExpander: actor_snapshot=malformed`; `GameHooks: No native GameType getter result or setter-cache entry`; and `AutoHit: feedback=no_immediate_hurt_time_transition: six accepted`.

## Interpretation against current experimental source

- `autohit.cpp` currently resolves the native GameMode and validates the public attack route before target selection. It does not call the GameType getter itself. The GameType warning is emitted from `GameHooks.cpp` when no getter or setter cache is available, so it may affect other modules (e.g. gamemode-aware target eligibility) but is not yet proven to be AutoHit's direct dispatch gate. Trace the Aim Assist actor filtering and PlayerGameType consumer before altering this fallback.
- `actor_snapshot=malformed` is a real risk to AutoHit only when the Hitbox Expander is enabled and its original target bounds cannot be recovered. Current AutoHit correctly fails closed in that case (`hitbox_original_bounds_unavailable`) rather than using expanded/invalid geometry. The warning alone does not prove the expander is enabled; the actual module-enabled state and `restore_query` detail are needed.
- `AutoTotem: tick_gate=disabled` is not evidence of an AutoHit dependency. Keep it isolated unless code tracing shows a shared tick gate or hook.
- The six-accepted/no-hurt-time warning is the most direct symptom. In current source, `GameHooks::attackAvailable()` accepts a validated public route even if the secondary internal observer cannot be installed; `attack()` then dispatches but only considers the result accepted when the observer saw the internal attack. Build #895 added this false-success guard. The new screenshot's `six accepted` wording may come from a different/older installed build or from a different path; capture exact build/version and fresh JSON before assuming it includes Build #895.

## ShadowHook / Apollon research

- Upstream ShadowHook is ByteDance's Android inline-hook library: https://github.com/bytedance/android-inline-hook. It supports ARM32 and ARM64 and hooks a function by address or symbol. Its manual explicitly requires the proxy to use the same ABI (argument count/order/types and return type) as the original function. It can improve hook installation/observability, but does not discover the correct Minecraft function, recover a wrong offset, or make a guessed ABI safe.
- The current public Apollon repository `TheChuan1503/apollon-client-for-minecraft-pe` does **not** reference ShadowHook in its checked source tree or `main.cpp`. `main.cpp` includes Dobby (`Includes/Dobby/dobby.h`) and `Arm64/And64InlineHook.hpp` for ARM64 (Substrate for other ABIs). Thus the claim that this Apollon revision uses ShadowHook is not confirmed by the public source inspected; another fork/revision may differ.
- Apollon's `main.cpp` has several direct `GameMode::attack(target, true)` calls. In its `onTick`-style path, it sets `doHandleAttack = true` when native attack emulation is enabled; later in the client tick it calls `thiz->handleDestoryOrAttackButtonPress()` and clears the flag. This suggests the intended animation/input path is tied to the ClientInstance attack-button callback, not an arbitrary animation-only effect. However, Apollon uses hard-coded offsets and its existing native hook library; do not copy those offsets or call the method in Bedaim until the method is resolved and ABI-validated against the exact installed Minecraft binary.

## Safe next steps

1. Do not add ShadowHook just to chase damage: first compare Bedaim's existing hook backend and test whether the observer can be installed on the exact validated attack implementation. ShadowHook is an alternative hook backend, not an attack implementation.
2. Add diagnostics separating: `public_route_validated`, `observer_install_attempted`, `observer_install_succeeded`, `observer_reached`, `public_return`, `native_return`, and an independently observed hit/swing animation. Never call a wrapper return 'damage'.
3. Inspect the exact `HitboxExpander` snapshot producer/restore query and the Aim Assist target snapshot producer. Do not consume malformed snapshots; when expander is enabled, require a validated original AABB.
4. Trace the GameType warning to its consumers. Add a safe, version-validated getter or keep unknown mode explicit; do not guess offsets or silently classify unknown as Survival.
5. Resolve `ClientInstance::handleDestoryOrAttackButtonPress` from the current binary via signature/control-flow validation, then compare manual attack vs AutoHit on a private test world. Only wire an input callback if its address, ABI, lifecycle and re-entrancy behavior are validated. Animation should be evidence of the real input path, not a cosmetic substitute for a successful hit.
6. Keep `main` untouched; work only in `perf/autohit-tick-throttle-20261019`. Build success is not proof of gameplay damage.