# Bedaim — swap transition timing and AutoHit route investigation (2026-10-20)

Branch: `experiment/swap-transition-autohit-diagnostics-20261020`
Base snapshot: `23c1ea23504866246cedea4c645045b9a1e6e663` from the AutoHit compile-fix experiment. This branch is experimental; do not merge. Preserve main, BedrockAim and BedrockBound.

## Evidence before source edits
- User's latest repro narrows MaceFall/ShiftBlade lag spikes to the moment the swapper changes the selected item to its representative item, rather than continuous idle ticks.
- Both modules call `bedrocktools::core::InventoryAccess::selectHotbarSlot(player, slot)`.
- In `src/core/InventoryAccess.cpp`, the exported `PlayerInventory::selectSlot(int, ContainerID)` path currently does:
  `const bool result = nativeSelectSlot(...); return result && selectedHotbarSlot(player) == slot;`
  This considers the native return value authoritative even though the selected-slot state is the observable postcondition. If the native operation changes the selection but returns false (or return ABI/meaning differs in this game build), the caller is told selection failed even though the item changed. MaceFall may then miss its pending-restore ownership state. This is a plausible correctness bug, not yet proven to be the FPS root cause.
- The same function has an existing direct-field + `PlayerSendInventory` fallback only when the native symbol is unavailable. Do not expand this fallback to a native-call failure without evidence.
- `maceswapper.cpp` and `swordswapper.cpp` both perform the selected-slot change from the verified local-player tick. No timing evidence yet distinguishes cost inside the native selector from Minecraft's own held-item transition/render work.

## Hypothesis recorded before source edit
A swap can report failure despite having changed the slot if the native selector's boolean return is false or inconsistent with the observed postcondition. Make the return condition depend on the observed slot after calling the native selector, and instrument slow native slot transitions. This is a narrow shared-path fix that should improve swap ownership/restore correctness and produce concrete timing evidence if the native selection is responsible for the spike. It may not eliminate lag if the engine's item transition itself is expensive.

## Intended tests
1. Edit only `src/core/InventoryAccess.cpp` for this patch; preserve the existing fallback behavior.
2. Make native selection success depend on `selectedHotbarSlot(player) == slot`, not the returned bool alone.
3. Record elapsed time for the native select call and total selection method; emit a warning only if total selection exceeds 2 ms or native call exceeds 2 ms. Include before/target slot, native result, observed state, and duration. Keep normal tick logs silent.
4. Run host regression tests and Android ARM64 build/package via GitHub Actions. Require an actual successful workflow run and `BedrockTools.levipack` artifact; no build claims until verified.
5. On device, test MaceFall and ShiftBlade separately with diagnostics enabled and compare the new slow-selection entries to the exact frame spike. Preserve and test BedrockAim/BedrockBound.
6. For AutoHit, treat source compile fixes and actual attacks as separate goals. Prior diagnostics show public GameMode attack resolution can fail and previous wrapper returns did not prove damage. Do not guess a vtable slot or ABI. A reliable native-route fix requires fresh diagnostics and the exact `libminecraftpe.so` / APK matching the currently tested Minecraft build.

## AutoHit source facts before any route edit
- `autohit.cpp` checks weapon profile, resolves the player-owned GameMode, calls `attackAvailable(gameMode)`, then obtains the current BedrockAim target snapshot, validates bounds/reach/cooldown, and calls `gamehooks::attack(..., &hitPoint, &nativeAttackResult, &nativeAttackResultUsed)`.
- `GameHooks::attackAvailable` validates the live vtable through `publicAttackVirtualEntry`; it does not simply call a guessed fixed slot.
- The route resolver tries exported public three-argument symbols or a verified ARM64 adapter/branch relationship, then a guarded four-argument route validated against independently resolved signatures.
- Historical library diagnostics from 2026-10-19 contain inconsistent route results and zero hurt-time changes after purported accepted wrapper returns. They are historical and not proof of the current Build #898 or current source behavior.
- The current repository does not contain the user's target `libminecraftpe.so` in the mounted working directory. No ABI change is safe to make from guesswork.

## Starting state
- This is a new experimental branch based on the current source-fix experiment. No source file has been edited in this cycle yet.
- The parent AutoHit compile-fix PR #43 is open/draft and unmerged.
- `main` remains untouched.


## First patch implemented — validation pending
- Changed `src/core/InventoryAccess.cpp` only. Native `selectSlot` now determines success by the observed selected-slot postcondition (`selectedHotbarSlot(player) == slot`) rather than requiring both the native bool and the postcondition. The existing direct-field + `PlayerSendInventory` fallback is unchanged and still runs only when the native symbol is unavailable.
- Added slow-transition diagnostics only when either the native selector or the total selection method takes at least 2,000 microseconds. The message includes route, previous/requested/observed slot, native return, native duration and total duration. Fast slot changes remain silent.
- Source commit: `130c5c46581bb22345ababdc6fbe661afdb544c0` — https://github.com/wendellestumano/Bedaim/commit/130c5c46581bb22345ababdc6fbe661afdb544c0
- Expected risk: this does not avoid the game's own item-model/equip work; it corrects a return-state mismatch and adds measurements. Runtime profiling is needed to determine whether native selection explains the FPS spike.
- Current tests: not yet run. No local SDK/NDK/build runtime is attached to this session. The parent contains the AutoHit/GameHooks diagnostics compile fixes but their Android build is not confirmed. No new `.levipack` exists for this commit yet.
- Changed code file: `src/core/InventoryAccess.cpp`; progress file: `docs/PROGRESS_SWAP_AUTOhit_20261020.md`. BedrockAim, BedrockBound, MaceFall and ShiftBlade logic were not edited in this patch.


## Combined AutoHit + swap transition experiment — pre-edit record (2026-10-20)

### Binary evidence and hypothesis (before source edit)
- Local target file observed in the connected Termux: `~/storage/shared/Download/libminecraftpe.so`, 328,419,680 bytes, identified as the user's Minecraft Bedrock 26.51 binary.
- On that binary the existing `SignatureId::GameModeAttack` and `GameModeAttackInternal` patterns both resolve to `0xF89FA58`. Disassembly there uses `x3` as a bool output pointer and clears `*x3`.
- A distinct public 3-argument adapter is at `0xF89F6B4`: `mov x3,x2`, `mov w2,#1`, `b 0xF89F6C0`. The target at `0xF89F6C0` uses the fourth machine-register argument as the hit-position pointer. The existing `isPublicAttackAdapter()` only accepts branch targets matching the two helper signatures, so it cannot recognize this public adapter on the supplied binary. This is a concrete source/binary mismatch hypothesis for AutoHit route discovery; it is not proof of server-side damage.
- Hypothesis: add an independently resolved signature for the public adapter destination and use it only to validate the public wrapper relationship. Preserve the two existing helper signatures and the guarded four-argument path, avoid fixed vtable slots, and let the resolver select the actual live matching virtual entry.
- Binary scan expectation: the new target signature must match exactly once at `0xF89F6C0`, while the wrapper's argument-shuffle/branch sequence matches exactly once at `0xF89F6B4`.

### Swap transition hypothesis (before source edit)
- User repro narrowed FPS spikes to the moment MaceFall/ShiftBlade changes selected item. The current `InventoryAccess::selectHotbarSlot` patch logs only calls exceeding 2 ms; a quick selector followed by an expensive engine equipment/render transition could remain invisible.
- Hypothesis: record each actual slot transition at info level and retain warning level for native or total method duration >=2 ms. Keep unchanged fast calls silent. This helps separate slow Bedaim selection from downstream game work and is not yet claimed to fix the FPS issue.

### Intended tests
1. Apply only the exact public-adapter signature/resolver change in `Signatures.hpp`, `Signatures.cpp`, and `GameHooks.cpp`; preserve the existing output-bool route.
2. Apply only transition-event logging changes to `InventoryAccess.cpp`, without adding a per-tick trace or changing the symbol-missing fallback.
3. Run all seven host regression targets; run syntax checks for changed translation units where supported.
4. Obtain a real GitHub Actions Android ARM64 result and require successful packaging of `BedrockTools.levipack`. Host tests or syntax checks alone are not an Android package build.
5. On the exact Minecraft 26.51 runtime, collect fresh diagnostics with AutoHit enabled and no manual attacks. Validate actual target health/damage separately from native return, observer result, particles, or immediate hurtTime.
6. Test MaceFall and ShiftBlade separately, compare `hotbar_slot_select` duration and timestamp with frame spikes. Do not attribute improvement without in-game timing evidence.

No source code has been edited on this new remote branch yet. `main`, BedrockAim and BedrockBound are to remain untouched; no merge is planned.


## Source edit results — 2026-10-20 (remote experimental branch)
- Applied the pre-recorded AutoHit resolver hypothesis in three files. New signature ID: `GameModeAttackPublicAdapterTarget`; signature pattern: `FF 43 07 D1 FD 7B 18 A9 FC CB 00 F9 F8 5F 1A A9 F6 57 1B A9 F4 4F 1C A9 FD 03 06 91 58 D0 3B D5 F3 03 00 AA F4 03 03 AA`. It is added only to the allow-list inside `isPublicAttackAdapter()`; the four-argument guarded validator and the existing bool-out signatures remain unchanged.
- Applied the pre-recorded swap logging hypothesis in `src/core/InventoryAccess.cpp`. Actual observed slot transitions now emit `hotbar_slot_select` at Info level even when the selector is fast. Native or total duration >=2,000 microseconds emits Warning. Unchanged fast calls remain silent. Native selection success is still decided by the observed selected-slot postcondition; no unsafe fallback was added after native-call failure.
- Remote source commits: enum ID `9d9bfe47ab04ac31923f70d88fdc151e4b64b982`; signature `7f85a160214b2e6d88c68de8a76161569a80a8a4`; resolver `c636bcabc011bd334c0dba20d025a86b6a0f3e39`; inventory transition diagnostics `973e163c2c3b70a721dcee1f603be382a6152503`. They are on `experiment/autohit-public-adapter-26_51-ci-20261020`; no merge.
- Local source mirror commit (not pushed by git): `2166ff3d5d93be81da42b23d3f4173ede2bff5bc`. The Termux push attempt did not complete, so remote API commits above are the authoritative GitHub branch state.
- Local validation on the matching source changes: `git diff --check` passed; the target signature matched once at `0xF89F6C0`, and wrapper shuffle/branch bytes matched once at `0xF89F6B4`. Host xmake test project built successfully; seven tests ran, total process exit code 0: menu config 64/0, AimAssist math 27/0, atomic writer 10/0, diagnostics exit 0, AutoHit guards 48/0, weapon profile exit 0, AutoTotem policy 14/0. Syntax-only checks passed for `Signatures.cpp`, `GameHooks.cpp` (using existing Termux EGL stub), and `InventoryAccess.cpp`.
- Limitations: these checks are not Android ARM64 linking/package output. Termux has no Android NDK/SDK installed, so no local `.levipack` was generated. No new GitHub Actions run has been observed for this remote branch yet. No in-game runtime evidence exists for actual AutoHit damage or FPS improvement.
- Next step: open a draft PR against `experiment/swap-transition-autohit-diagnostics-20261020` to trigger the normal `pull_request` build, then inspect the actual Android compile log and uploaded `BedrockTools-levipack` artifact. Keep the PR unmerged until clean game testing with fresh logs confirms AutoHit damage and quantifies slot-transition spikes.


## CI update — PR #45 / Build #916
- Draft PR #45: https://github.com/wendellestumano/Bedaim/pull/45
- Workflow run #916: https://github.com/wendellestumano/Bedaim/actions/runs/39431784071
- Observed workflow state: host regression tests completed successfully; NDK setup completed successfully; Android ARM64 Build and package was still in progress at this update. Artifact list was empty while the job was running. No build pass or levipack artifact is claimed yet.
- Next check: inspect the final job conclusion and artifact list; if it fails, fetch the exact compiler log and make a focused correction, recording a new hypothesis before any further source edit.


## AutoHit crosshair-only gate — pre-edit hypothesis/test record (2026-10-20)

### Evidence
- User explicitly requests normal attack activation only when the current target is inside the camera crosshair and at most 3 blocks away.
- `src/modules/player/aimassist.hpp` already exposes `AimAssistModule::currentTargetInCrosshair(player, target, tolerance)`. Its implementation performs a ray/AABB intersection from the player's current view rotation and can enforce line of sight according to AimAssist settings.
- `currentTargetInCrosshair` currently requires `AimAssistModule::enabled`, but the module's own event system deliberately continues tracking while AutoHit is enabled (`autoHitNeedsTargeting()`). Thus AutoHit-only mode needs this gate to accept that same AutoHit-driven targeting condition; otherwise the gate rejects all targets when AimAssist itself is toggled off.
- The existing AutoHit dispatch does not call an explicit engine input callback. Its logs state `explicit_input_callback=not_called_in_this_path`, and 82 observed `GameHooks` dispatch entries in the exported diagnostics used the guarded four-argument route with `native_out_result=false`. These are separate problems: first prevent attacks unless the crosshair/reach gates pass, then separately validate the correct normal input callback ABI.
- Prior Apollon source names `ClientInstance::handleDestoryOrAttackButtonPress`, but its hard-coded address for Minecraft 26.51 is stale; no copied offset will be used.

### Hypothesis (before source edits)
1. Add an early AutoHit gate invoking `currentTargetInCrosshair(player, target, 0.12f)` after current target/hitbox validation and before reserving CPS or dispatching any attack. This should prevent attacks at stale targets behind/outside the crosshair while preserving the existing target provider and three-block reach gate.
2. Adjust only `AimAssistModule::currentTargetInCrosshair`'s enabled predicate to use the same `(!enabled && !autoHitNeedsTargeting())` condition already used by snapshot/bounds methods. This lets AutoHit use the ray test with AimAssist's steering disabled, without enabling AimAssist steering.
3. Default reach is 3.0 blocks. Preserve explicit user-configured values in existing configs, clamped to the existing 2–6 block range.
4. Do not change attack dispatch ABI or normal-input callback route in this patch until a version-verified callback signature and invocation context are established.

### Intended tests
- Add focused host tests for ray/AABB crosshair gate (centered hit, target offscreen/behind, target outside 3 blocks, stale target) if the existing test layout can test the pure geometry without requiring the Minecraft runtime.
- Run all seven existing host regressions.
- Run Android ARM64 CI and confirm a generated `.levipack` artifact; inspect actual job logs.
- Device validation: centered target within 3 blocks should reach the input/dispatch path; behind/offscreen target must produce a gate reason and zero dispatches. AutoHit-only mode (AimAssist disabled) must still run the crosshair predicate.
- Normal button callback remains a follow-up implementation; don't mark AutoHit fully fixed until a real input callback is invoked and actual target damage is confirmed.

This is a new experimental branch based on the current public-adapter/swap-tracing experiment; no source code has been edited on this branch yet. `main`, BedrockAim steering defaults, and BedrockBound remain untouched; no merge.


## Reach default edit — post-gate correction
- Source: `src/modules/player/autohit.cpp` default `configuredRange` and `src/modules/player/autohit.hpp` default `m_attackRange` changed from 3.2 to 3.0 blocks. Existing saved configuration is still honored by `loadConfig`, and the setting remains configurable in the existing 2–6 block slider.
- This aligns the default configured reach with the requested 3-block radius; it does not clamp every user's saved setting to 3.0.
- Tests and Android build for this latest edit are pending; do not claim a new artifact until CI confirms it.


## PR #46 / Build #920 — verified package result (2026-10-20)
- PR: https://github.com/wendellestumano/Bedaim/pull/46 (open, draft, not merged); branch `experiment/autohit-crosshair-gate-20261020`; HEAD at this CI run `215822627ff92b0a9fad5b170d13fceb1b0f7f27`.
- Workflow: https://github.com/wendellestumano/Bedaim/actions/runs/39447621275 — final job conclusion `success`. Host regression suite passed (menu config 64/0, AimAssist math 27/0, atomic writer 10/0, diagnostics exit 0, AutoHit guards 48/0, AutoHit weapon profile exit 0, AutoTotem policy 14/0). NDK setup completed; Android ARM64 Build and package completed; artifact upload completed.
- Artifact: `BedrockTools-levipack`, ID `12277168057`, upload size 688,055 bytes, SHA256 of uploaded artifact zip `68f63a7dd0d619799459a6a98a77df28ca30de39347598cbf2b1094274ac649f`; download page https://github.com/wendellestumano/Bedaim/actions/runs/39447621275/artifacts/12277168057.
- Downloaded and verified inner `/mnt/data/BedrockTools-build920.levipack`: 689,656 bytes, SHA256 `c95d631104f7f9fce41acbf5b4114a83ef5d973e0053ef56ee3f2a153911f41f`; contains manifest.json (358 B, SHA256 `8d43254ab345298d0833dbbe98bfb751c999d84f267eee202d6170eadc455e10`), libBedrockTools.so (1,326,080 B, SHA256 `5e272805bb22771518764965b757fb813fa9730824005094c73116557d5810b9`), icon.png, and resources/minecraft.ttf. Manifest identifies BedrockTools 1.5.7 / preload-native.
- Important: this proves host tests and Android ARM64 compilation/package for the crosshair-gate + 3.0 default reach revision. It does not prove crosshair gating in actual gameplay, normal engine attack-button callback invocation, actual damage, or FPS improvement.
- Next: install this pack on the matching Minecraft 26.51 setup and test (1) crosshair centered target <=3 blocks, (2) behind/offscreen target must emit `target_not_under_crosshair` and no attack dispatch, (3) AutoHit-only with AimAssist steering disabled, (4) BedrockAim/BedrockBound still work. Save fresh Module Diagnostics JSON afterwards. Continue separate exact-binary analysis for `ClientInputCallbacks::handleDestoryOrAttackButtonPress`; do not copy Apollon's stale offset `0x6462B60`.


## 2026-10-21 — AimAssist actor-vector capacity validation

- Audited against the last valid package, Build #948 (base source SHA `77694ace6b790f138a1cba468d53cffbae08aed0`; workflow run `39518467697`, artifact `12310400412`). The current tick path validated begin/end and element count but omitted capacity; other consumers already checked capacity, so guard coverage was inconsistent.
- Experimental fix: centralize address-only metadata validation in `src/modules/player/ActorVectorValidation.hpp`, use it before any actor-vector iteration in AimAssist, target-bounds lookup, nearby-actor collection, and Hitbox Expander. Validate metadata before classifying an empty snapshot, so an empty range with an invalid/oversized capacity is rejected safely.
- Added host regression cases for all-null/valid-empty vectors, valid nonempty vectors, null/reversed pointers, capacity before end, misaligned spans, element/capacity limits, and zero stride. Added the test target to the existing host test workflow.
- No native pointer is dereferenced by the validator. Runtime safety and in-game AimAssist/Hitbox behavior still require CI and device validation; no FPS improvement is claimed.
- Branch: `experiment/aimassist-actor-vector-capacity-guard-20261021`. Base SHA: `77694ace6b790f138a1cba468d53cffbae08aed0`. CI/levipack status: pending after this commit; final run and artifact will be linked in the experimental PR conversation without a follow-up source commit.
