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
