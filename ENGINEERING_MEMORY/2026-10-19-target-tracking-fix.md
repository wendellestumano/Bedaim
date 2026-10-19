# 2026-10-19 — Target-tracking gate fix

## Scope
- Repository: wendellestumano/Bedaim
- Branch: perf/autohit-tick-throttle-20261019
- Code commit: 49543f9c3b6b3cc9e561f8072bf5e4d00f7f111d
- No edits were made to main.

## Finding
In src/modules/player/aimassist.cpp, autoHitNeedsTargeting() only allowed target acquisition while AutoHit was enabled if gameMode() and attackAvailable(gameMode) were already true. A failed attack-route check could therefore also stop fresh target snapshots and rotation. The helper now depends on AutoHit enabled state only. The dispatch path in autohit.cpp still independently requires attackAvailable(gameMode); no unvalidated native calls were enabled.

## Tests
- aimassist_math_test: 27 checks, 0 failures.
- autohit_guards_test: 21 checks, 0 failures.
- autohit_weapon_profile_test: compiled and exited successfully; no check-count text was emitted.
- Static checks passed: tracking predicate has no gameMode/attackAvailable gate; attack dispatch still has both route validation and its existing invocation.
- Tests were built with Termux clang++ from the isolated Testing2 source copy after mirroring this one-block edit. The Android shared-storage directory is no-exec, so binaries were copied to private Termux storage before execution.
- Full Android .levipack build was not completed. Xmake configuration could not install its Git dependency (Termux reported nix unavailable). CI status for the code commit returned no checks; no workflow run was returned.
- In-game result, actual damage, and lag are unverified. This is a target-tracking fix, not a confirmed AutoHit fix.

## Apollon review
The public Apollon source uses a queued native input callback in its emulateNativeAttack mode: target selection sets a pending flag; a ClientInstance tick consumes that flag with handleDestoryOrAttackButtonPress(). This differs from a direct GameMode attack call and merits further investigation.

Do not reuse Apollon offsets blindly. In the 1.26.52.3 target binary, the old address 0xCCC075C disassembles in Java_com_mojang_minecraftpe_AppExitInfoHelper_nativeSendApplicationExitInfo, and 0x6BEDA24 is in a cohtml::css::CustomExpressionResolver::Solve region. The handler's old offset was not validated. We have not identified or called the equivalent target-build input handler yet.

## Remaining blockers
- Current runtime reports no validated public GameMode attack vtable route.
- ClientInstance vtable getter slot 32 frequently returns null; other logs show it sometimes captures a LocalPlayer.
- Some diagnostics call the attack accepted but show no immediate hurt-time increase; this is not confirmed damage.
- Next: validate the current target-build input handler and GameMode route using exact binary/vtable evidence, reconcile the build/source mismatch in no-hit quarantine logs, then build and conduct a controlled private-world damage test.
