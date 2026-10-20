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
