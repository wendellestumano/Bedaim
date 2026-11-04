# ItemDuplicator Research Plan — Minecraft Bedrock 26.60

Status: preparation / read-only research phase
Branch: research/itemduplicator-inventory-observer-26.60
Baseline: main @ ac1386dd79effd90b9dfce82225a02b7c0847776
Target: LeviLauncher Android, arm64-v8a, Bedrock 1.26.60.x

## Goal

Build a native C++ Bedaim module that observes inventory state transitions and determines whether a suspected duplication is real, client-only, or rejected/corrected by the authoritative server. Initial scope is research and reproducible testing in a local world and a server explicitly controlled or authorized by the user.

This phase is observational. It must not forge inventory transactions, replay requests, suppress server corrections, or automate exploit attempts against public servers. Any later experiment that changes inventory state must be explicitly opt-in and restricted to an isolated test world/server with disposable items and backups.

## Repository findings

- The project is a C++20 Android native mod built with xmake and Preloader.
- The build compiles `src/modules/**.cpp`; a new module can be added without a new build target.
- `ModuleRegistry::registerAllModules()` is the central module registration point.
- `Module` provides lifecycle callbacks and JSON config serialization.
- `Runtime::wireEvents()` publishes frame and input events. `GameHooks.cpp` publishes `LocalPlayerPreTickEvent`, `LocalPlayerTickEvent`, and `ContainerSlotSelectedEvent`.
- `InventoryAccess` already reads item identifiers/counts and selected hotbar slot, but its native layout assumptions must be revalidated for 1.26.60 before adding new dereferences.
- The current `include/bedrocktools/sdk/network/Packet.hpp` is a generic packet wrapper; it does not establish a verified inventory packet hook or a supported transport interception point.
- `levimod.json` targets `1.26.60.X`. The current package workflow emits `Bedaim.levipack`.

## Proposed module

Runtime module ID: `bedrocktools.Item Duplicator`
Class: `ItemDuplicatorModule`
Files:
- `src/modules/misc/itemduplicator.hpp`
- `src/modules/misc/itemduplicator.cpp`
- `src/modules/misc/InventorySnapshot.hpp`
- `src/modules/misc/InventorySnapshot.cpp`
- `src/modules/misc/InventoryResearchLog.hpp`
- `src/modules/misc/InventoryResearchLog.cpp`

The module should be disabled by default. Its first feature is a read-only observer, not an item mutation routine.

## Data model

Each snapshot should contain:
- monotonic timestamp (steady clock) and wall-clock timestamp for log correlation;
- session/world identifier when safely available (otherwise a generated session token);
- selected hotbar slot;
- for each readable inventory slot: slot index, item identifier, stack count, validity/readability flags;
- whether the player is in a local world or connected session, only when a trustworthy runtime signal exists;
- source event (pre-tick, post-tick, container selection, explicit manual capture).

Never serialize raw pointers, memory addresses, account identifiers, player names, server addresses, or item NBT/content. Treat unreadable or unresolved data as `unknown`, never as an empty slot or zero count.

## Observation pipeline

1. Capture a baseline snapshot on the first safe post-tick after the module is enabled.
2. Capture snapshots after relevant inventory/container events and at a bounded cadence (avoid per-frame disk writes).
3. Diff adjacent snapshots by slot and item identifier.
4. Correlate local changes with observed native events and, if a stable supported observer is identified, inventory request/response metadata.
5. Persist a bounded in-memory event ring and flush JSONL records on a worker thread using atomic file replacement or append-safe writes.
6. On reconnect/reopen, compare a fresh server-authoritative state where available.

## Classification rules

- `LOCAL_ONLY`: local display changes, but authoritative state is not confirmed.
- `SERVER_CORRECTED`: a later authoritative update restores/rejects the local change.
- `PERSISTED`: the expected item counts remain after closing/reopening the container and reconnecting to the same test world/server.
- `INCONCLUSIVE`: missing events, unreadable memory, session transition, or insufficient evidence.

A real duplication finding requires two independently persisted copies of the same item lineage/count after a clean reconnect or world reload. A temporary client-side count, packet observation, or UI screenshot is not proof.

## Version 26.60 compatibility gates

Before reading any new inventory field or installing a hook:
- confirm the exact game build string at runtime;
- validate each object/field pointer with the existing safe-read approach (or a reviewed extension);
- verify the item-stack layout and container bounds against 1.26.60 binaries/symbols;
- install hooks only when their signatures and expected function ownership validate;
- fail closed when a signature, pointer, or event source is unknown;
- do not assume RakNet-only interception: the transport may vary by client/server version.

No packet mutation, request replay, transaction duplication, or correction suppression is in this phase.

## UI/config proposal

- Enable Inventory Observer (default false)
- Capture interval (default 250 ms; bounded 100–2000 ms)
- Capture container events (default true)
- Capture hotbar selection (default true)
- Persist research log (default true)
- Maximum in-memory records (default 512)
- Export latest session (action)
- Clear current session log (action with confirmation)

Show counters for snapshots, detected changes, unreadable slots, and classifications. Never label a result as a successful dupe until the persistence criterion above is met.

## Test matrix

### Local world
- move one item between hotbar and inventory;
- split and merge stacks;
- move items into and out of containers;
- close/reopen inventory and containers;
- save/exit/reload world;
- compare snapshots and confirm that ordinary actions produce no false-positive duplication.

### Authorized server
- repeat the same benign inventory operations;
- record local transitions and server corrections;
- disconnect/reconnect and inspect the authoritative inventory;
- use disposable test items and a private test server/world backup;
- do not run experiments on public servers or affect other players.

### Regression / build
- host unit tests for snapshot diff/classification independent of Minecraft memory;
- Android ARM64 release build;
- inspect .levipack contents;
- launch with observer disabled and confirm no change to existing module behavior;
- enable observer and verify bounded CPU, memory, and disk usage.

## Implementation sequence

1. Add pure C++ snapshot/diff/classification types and host tests.
2. Add a safe read-only inventory snapshot API to `InventoryAccess`, only after 1.26.60 layout validation.
3. Add `ItemDuplicatorModule` and register it in `ModuleRegistry`.
4. Add Mod Menu settings and persistent JSONL export using existing diagnostics/config patterns.
5. Add optional event correlation for container selection and local-player tick events.
6. Investigate inventory packet observability separately; do not install an unverified packet hook.
7. Run local and authorized-server test matrix; document confirmed, rejected, and inconclusive results.

## Acceptance criteria

- MAIN remains untouched.
- Observer is off by default and cannot mutate item stacks.
- Unknown/unreadable states fail closed.
- No false claim of duplication based solely on client-side state.
- Host tests pass and Android ARM64 .levipack builds.
- Existing Bedaim modules continue to initialize and run unchanged.


## 26.60 binary reconnaissance (2026-11-04)

A read-only string scan of the user's local `libminecraftpe.so` for Minecraft 1.26.60 found the following runtime names and validation/error paths:

- `ItemStackNetManagerClient` and `ItemStackNetManagerServer`;
- `ItemStackRequestPacket`, `ItemStackResponsePacket`, and `InventoryTransactionPacket`;
- client request/response handling and server request queue/processing;
- server-side checks including `_isRequestActionAllowed`, `_validateRequestSlot`, net-ID mismatch, `SourceItemMismatch`, and `BalanceMismatch`.

This confirms that 1.26.60 has explicit request validation and authoritative transaction handling. It does not provide function addresses, class layouts, or a safe hook signature. A symbol-table query did not expose matching demangled symbols, so the strings are leads only and must not be treated as ABI proof.

Implementation consequence: begin with pure snapshot/diff/classification logic and read-only inventory observation. Do not infer a dupe primitive from packet class names or attempt to bypass the server's balance/net-ID checks.

## Initial pure-logic implementation

Added `src/modules/misc/InventoryResearch.hpp` and a host test target `inventory_research_test`. The classifier distinguishes unchanged state, client-only count increase, server correction, persisted count increase after reconnect, and inconclusive evidence. Its strongest positive label is intentionally `PersistedCountIncrease`, not "dupe", because count-only snapshots do not prove item lineage or independent copies.


## Preparation verification

- Experimental branch checked out successfully in a Termux-private working directory. A first clone directly into shared storage failed because the repository contains a legacy filename that Android shared storage rejects; cloning under Termux home succeeded.
- Compiled `tests/launcher/inventory_research_test.cpp` directly with Termux `clang++ -std=c++20 -Wall -Wextra -Werror`.
- Result: `Inventory research classification tests passed`.
- This validates only the pure evidence classifier. It is not an Android game build and does not validate runtime memory offsets, hooks, UI registration, or a working duplication mechanism.
