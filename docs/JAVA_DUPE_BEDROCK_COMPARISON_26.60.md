# Java Dupe Research vs Bedrock 26.60

Date: 2026-11-04
Repository: wendellestumano/Bedaim
Branch: research/itemduplicator-inventory-observer-26.60

## Scope

This report compares the architecture of public Minecraft Java client/mod research projects with the Bedrock 1.26.60 inventory protocol. It is intended to guide a read-only observer and controlled experiments on a local world or private server controlled by the user. It is not a recipe for exploiting third-party servers.

## 1. Java mod research

### LiquidBounce

Reviewed the public source for:
- `ModuleDupe.kt`
- `DupeExploit.kt`
- `DupePaper1204.kt`

Findings:
- The module rejects local-server and Creative sessions, disables on quit, and does not include its one-shot state in automatic configs.
- It uses an exploit-mode abstraction; the documented implementation is explicitly for Paper 1.20.4 through roughly 1.21.1.
- The Paper implementation depends on a writable-book precondition and a book-edit request with an over-limit title. Applicability depends on a specific server-side Paper behavior; it is not a generic inventory duplication API.
- It is not portable to Bedrock by copying a packet. Bedrock uses a different packet family, action schema and server implementation.

### PacketManipulator

Reviewed:
- `MixinClientConnection`
- `FabricateScreen`
- `MixinClientPlayerEntity`

Findings:
- The Java mod intercepts outgoing `ClientConnection.sendImmediately` to allow selected outgoing packets to be cancelled.
- Its fabricator exposes Java `ClickSlotC2SPacket` fields such as window/sync ID, revision, slot, button and action.
- The packet fabricator is a generic diagnostic/research tool, not evidence that arbitrary slot-click packets duplicate items.
- Its tick mixin delegates to a client mod callback; a packet/tooling layer still needs a server-side defect to produce an authoritative duplication.

### DupeClient

Its public Packet Utils documentation describes queue/delay controls, packet fabrication, packet sniffing, filtering, blocking, replay and packet-diff tooling. These are useful research/instrumentation patterns, but do not constitute a universal dupe implementation.

## 2. Bedrock 1.26.60 protocol comparison

Mojang's protocol changelog records Bedrock 1.26.60 as released 2026-10-27 with protocol version 2225. Version 1.26.61 uses the same protocol number and records zero packet changes.

The 1.26.60 protocol schema has:
- `ItemStackRequestPacket` (ID 147): batched client requests, each containing a client request ID and typed action list with source/destination container and slot information.
- `ItemStackResponsePacket` (ID 148): server response with a result enum and container/slot results on successful requests.

The response schema enumerates failures including invalid item net IDs, disallowed actions, transfer-amount problems, source/destination validation failures, invalid request states and other transaction errors.

Mojang's server-auth inventory guide describes client prediction for UI operations, authoritative server success/failure responses, committing predictions on success and discarding the relevant predictions on failure. Clients may issue subsequent requests before prior responses arrive for UI responsiveness; this behavior alone is not evidence of an exploitable race.

### Bedrock book editing

Bedrock's `BookEditPacket` (ID 97) has a book slot and a tagged operation variant:
- `ReplacePage`
- `AddPage`
- `DeletePage`
- `SwapPages`
- `Finalize`

This is structurally different from Java's `ServerboundEditBookPacket`. The Java Paper over-limit-title routine cannot be transplanted verbatim. The schema alone does not establish that a Bedrock server has a comparable validation flaw.

### Player UI container

The protocol guide describes a dedicated 51-slot Player UI Container for cursor and screen-specific inputs/outputs (crafting, anvil, loom, etc.). The server tracks and persists relevant UI item stacks as part of its container lifecycle. This is a useful area for lifecycle and persistence observation, not proof of a current duplication bug.

## 3. Adaptation matrix

| Java concept | Bedrock adaptation |
|---|---|
| Packet event capture / packet diff | Potentially reusable as an observer after a version-verified packet dispatch hook is found |
| Server-specific exploit-mode abstraction | Reusable as a design pattern only |
| Java window ID + revision click model | Not portable; Bedrock uses container names, request IDs, action batches and stack net IDs |
| Java book-edit packet | Not portable; Bedrock has a different BookEdit operation schema |
| Local inventory prediction | Reusable as an observation point, but must be compared with server-confirmed state |
| Arbitrary packet replay/fabrication | Excluded from the initial Bedaim implementation |

## 4. Implementation decision

No Java source inspected establishes a universal dupe algorithm. The LiquidBounce routine relies on a Paper-specific behavior; Java click-packet tooling is a test instrument, not a dupe by itself. Bedrock 26.60's server-authoritative request/response and net-ID checks have no direct Java equivalent.

The first executable Bedaim module will therefore be a read-only held-item/inventory observer and evidence logger. A genuine Survival duplication routine will not be inferred from packet names or copied from Java. It requires an independently reproduced defect in a Bedrock 1.26.60 local/private test server, with persistence verified after reconnect or world reload.

## 5. Reference sources

- LiquidBounce public source: `CCBlueX/LiquidBounce`, `ModuleDupe.kt`, `DupePaper1204.kt`.
- PacketManipulator public source: `Gulag-Innovation/PacketManipulator`, `MixinClientConnection.java`, `FabricateScreen.java`.
- DupeClient public docs: Packet Utils and DupeDB.
- Mojang Bedrock Protocol Reference, stable 1.26.60 / protocol 2225:
  - `ItemStackRequestPacket` ID 147.
  - `ItemStackResponsePacket` ID 148.
  - `BookEditPacket` ID 97.
  - Server-authoritative inventory and Player UI Container guides.
