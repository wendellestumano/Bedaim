# 2026-10-19 — Runtime log/source reconciliation

## Exact references
- Repository: `wendellestumano/Bedaim`
- Branch: `perf/autohit-tick-throttle-20261019`
- Latest confirmed source commit before this note: `49543f9c3b6b3cc9e561f8072bf5e4d00f7f111d`.
- Engineering-note head before this note: `8005769af1d17b01b4f70b598d6a34ba1106ed95`.
- `main` untouched.

## Critical discrepancy resolved
The diagnostic strings from earlier Build #844-era logs belong to an older source snapshot under Testing2, not the same source as the current GitHub branch.
- Older local snapshot logs: `No exact public GameMode::attack vtable match` and `target_quarantined_no_hit_feedback`.
- Current branch source includes `isGuardedFourArgumentAttackThunk` as Phase 2 of `publicAttackVirtualEntry`. When matched it emits `four_argument_native_attack_validated` and logs the matched vtable slot / implementation.
- Current branch's `AutoHitGuards.hpp` describes the no-hit-feedback observer as diagnostic only, and `autohit.cpp` emits one warning after six accepted calls with no immediate hurtTime increase, stating that attempts continue at configured CPS. There is no current dispatch gate on `m_noHitFeedbackGuard`.
- Therefore, do not repeat the old "quarantine stops attack" hypothesis for the new build unless a fresh post-install log reproduces it. Always record the source/build commit with each diagnostic export.

## Native four-argument route evidence
- Target binary `libminecraftpe.so` SHA-256: `53c4a86a8f3dbd7f0d48e440f8607d63c55100aeb364c80d19afefa8097e233a`.
- Historical log addresses normalize to candidate implementation `0xF89FE18` and vtable slot 2 thunk `0xF8A4048`.
- Target disassembly at `0xF8A4048`: initial `ldrb w8,[x0,#0xc8]`, conditional guards, an early false-return path, then `B 0xF89FE18` on the alternate path. The current source validator checks this guard/return/tail-branch shape and the target signature identity.
- Slot 16 (`0xF8A3FE0`) branches to `0xF89FA80`, so it is not the same attack implementation. Do not revert to slot-16 guesses.
- The native implementation contains explicit return paths setting `w0=0/1`, supporting—but not proving in gameplay—the bool-return ABI currently used. Server-side damage remains unverified.
- No blind offsets were imported into Bedaim.

## Apollon
- Public source was downloaded into Testing2 for reference. README states repository code is shared for reference and was not written by the repository owner; no explicit root license file was found in the archive.
- `emulateNativeAttack` queues `doHandleAttack`; a `ClientInstance_onTick` callback consumes it using `handleDestoryOrAttackButtonPress()`.
- Apollon's offsets do not validate against this target binary: `0xCCC075C` falls in a Java exit-info function region; `0x6BEDA24` is within `cohtml::css::CustomExpressionResolver::Solve`; `0x6462B60` lies before the target `.text` section. Do not copy these offsets. The current equivalent native-input handler remains unidentified.

## Build and test evidence
- GitHub Actions run for prior exact code commit `49543f9c3b6b3cc9e561f8072bf5e4d00f7f111d`: https://github.com/wendellestumano/Bedaim/actions/runs/39248087075 — success.
- Latest confirmed run before this note for source-identical documentation head `8005769af1d17b01b4f70b598d6a34ba1106ed95`: https://github.com/wendellestumano/Bedaim/actions/runs/39250253647 — host regression tests, NDK r28c setup, Android build/package, and artifact upload all succeeded.
- Artifact `BedrockTools-levipack` ID `12185566498`; artifact ZIP SHA-256 `08f2bd1c6b4af0d306be590e1ebae9ba8b767dc73561a1c010d5ca5943fc8cab`; inner `.levipack` SHA-256 `22712222a70806fa5abe9c60ad99a49aa6ab959b92a1867e82872b114cfc9ca7`; manifest version `1.5.7`.
- Host tests on the exact downloaded branch source all exited successfully: menu config 64/0, AimAssist math 27/0, atomic config writer 10/0, AutoHit guards 42/0, AutoHit weapon profile exit 0, diagnostics exit 0, AutoTotem policy 14/0. Official CI also reports the host-test step successful.
- No live game test performed. Do not declare AutoHit fixed.

## Next controlled test
Install this artifact only in a test environment, then verify in a private world with a damageable target. Fresh log should show `four_argument_native_attack_validated`, then `dispatching validated four-argument native attack route at slot 2` and an internal observer result if hook installation succeeds. Separately verify the target's health/damage—not particles or a native return value—and check for lag. If route validation is still absent, inspect the current live GameMode/vtable rather than changing offsets.
