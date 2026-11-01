#include "blockesp.hpp"
#include "BlockEventQueue.hpp"
#include "hitbox.hpp"
#include "../ModuleRegistry.hpp"
#include "../../core/Diagnostics.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/GameModeActionEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include "core/memory/Hooks.hpp"
#include <string>
#include <string_view>
#include <unordered_set>

namespace {
using BlockSourceGetBlockFn = void* (*)(void*, const bedrocktools::sdk::BlockPos&);
std::atomic<BlockSourceGetBlockFn> g_blockEspGetBlock{nullptr};
void* g_blockSourceGetBlockOriginal = nullptr;
bedrocktools::hooks::Handle g_blockSourceGetBlockHook = nullptr;
std::atomic<std::uint64_t> g_blockSourceGetBlockCalls{0};
std::atomic<std::uint64_t> g_blockSourceGetBlockCandidateCalls{0};
std::atomic<void*> g_blockEspActiveBlockSource{nullptr};
std::atomic<int> g_blockEspCaptureCenterX{0};
std::atomic<int> g_blockEspCaptureCenterY{0};
std::atomic<int> g_blockEspCaptureCenterZ{0};
thread_local bool g_blockEspInternalBlockLookup = false;
using BlockSourceGetBlockForTessellationFn = const void* (*)(void*, const bedrocktools::sdk::BlockPos*, std::int32_t);
void* g_blockSourceTessellationGetBlockOriginal = nullptr;
bedrocktools::hooks::Handle g_blockSourceTessellationGetBlockHook = nullptr;
std::atomic<std::uint64_t> g_blockSourceTessellationGetBlockCalls{0};
std::atomic<std::uint64_t> g_blockSourceTessellationCandidateCalls{0};
std::atomic<std::uint64_t> g_eventCandidatesQueued{0};
std::atomic<std::uint64_t> g_eventCandidatesDrained{0};
std::atomic<std::uint64_t> g_eventCandidatesDropped{0};
std::atomic<std::uint64_t> g_eventCandidatesClassified{0};
std::atomic<std::uint64_t> g_eventCandidatesMerged{0};
std::atomic<std::uint64_t> g_eventCandidatesNamedDrained{0};
std::atomic<std::uint64_t> g_eventCandidatesDedupBypassed{0};
std::atomic<std::uint64_t> g_eventCandidatesClassifyRejected{0};
std::atomic<std::uint64_t> g_eventCandidatesOutlines{0};

// Apollon's BlockESP observes block-update callbacks, queues only their BlockPos,
// then validates candidates against the live BlockSource before rendering them.
// Bedaim mirrors that split: the native hook is intentionally lightweight and
// never calls BlockSource or performs classification while inside the engine.
using BlockUpdateFn = void (*)(void*, void*);
void* g_blockUpdateOriginal = nullptr;
bedrocktools::hooks::Handle g_blockUpdateHook = nullptr;

// Apollon also hooks specialized block-event dispatchers. Their wrappers feed
// the same candidate routine as its generic update hook, but use two distinct
// position layouts (a BlockPos at +0x8 or a float Vec3 at +0x10).
constexpr std::size_t kApollonEventHookCount = 12u;
std::array<void*, kApollonEventHookCount> g_apollonEventOriginals{};
std::array<bedrocktools::hooks::Handle, kApollonEventHookCount> g_apollonEventHooks{};
std::array<std::atomic<std::uint64_t>, kApollonEventHookCount> g_apollonEventCalls{};
std::atomic<std::uint64_t> g_genericBlockUpdateCalls{0};

std::atomic<bool> g_blockEspCaptureEvents{false};

// Disabled by default while the 26.60 crash is investigated. These detours use
// Apollon-derived native addresses and event-object layouts; the bounded
// BlockSource scan and GameModeActionEvent queue remain available without them.
constexpr bool kEnableExperimentalNativeEventHooks = false;

constexpr std::uintptr_t kApollonBlockUpdateRva = 0x10F7FDE0u;

// Use the real ELF image base. BlockSourceGetBlock may move between Bedrock
// releases, so reconstructing the base from an RVA copied from Apollon's
// older engine can create an invalid pointer before the prologue guard runs.
bool getMinecraftImageBase(const void* address, std::uintptr_t& imageBase) noexcept {
    imageBase = 0;
    if (!address) return false;
    Dl_info info{};
    if (!dladdr(address, &info) || !info.dli_fbase || !info.dli_fname) return false;
    if (std::string_view(info.dli_fname).find("libminecraftpe.so") == std::string_view::npos)
        return false;
    imageBase = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    return imageBase != 0;
}

bool isAddressInMinecraftImage(const void* address, std::uintptr_t expectedBase) noexcept {
    if (!address || !expectedBase) return false;
    Dl_info info{};
    return dladdr(address, &info) && info.dli_fbase &&
        reinterpret_cast<std::uintptr_t>(info.dli_fbase) == expectedBase;
}
constexpr std::size_t kMaxEventCandidates = 4096u;

using BlockEventCandidate = bedaim::visual::BlockEventCandidate;
using BlockEventQueue = bedaim::visual::BlockEventQueue<kMaxEventCandidates, 8192u>;
BlockEventQueue g_blockEventCandidates;

void clearBlockEventCandidates() {
    g_blockEventCandidates.clear();
}

bool isValidEventPosition(const bedrocktools::sdk::BlockPos& position) noexcept {
    return position.x >= -30000000 && position.x <= 30000000 &&
           position.z >= -30000000 && position.z <= 30000000 &&
           position.y >= -4096 && position.y <= 4096;
}

void enqueueBlockEventPosition(void* source, const bedrocktools::sdk::BlockPos& position,
                               std::string_view resolvedBlockName = {}) noexcept {
    // Specialized Apollon dispatchers do not all pass the active BlockSource
    // as their second argument. Keep the pointer only as diagnostic metadata;
    // the candidate is revalidated against the current BlockSource on the tick.
    (void)source;
    if (!isValidEventPosition(position)) return;

    const auto outcome = g_blockEventCandidates.enqueue(
        source, bedaim::visual::BlockEventPosition{position.x, position.y, position.z}, resolvedBlockName);
    if (outcome.droppedOldest)
        g_eventCandidatesDropped.fetch_add(1u, std::memory_order_relaxed);
    if (outcome.inserted)
        g_eventCandidatesQueued.fetch_add(1u, std::memory_order_relaxed);
    if (outcome.merged)
        g_eventCandidatesMerged.fetch_add(1u, std::memory_order_relaxed);
}

// Broad fallback for supported block types that do not have an Apollon-specific
// dispatcher. Observe normal engine BlockSource lookups, but only queue blocks
// outside the immediate player area; the existing local scanner remains the
// near-player path. Internal BlockESP lookups are guarded to prevent recursion.
void* blockSourceGetBlockHook(void* blockSource,
                              const bedrocktools::sdk::BlockPos& position) {
    const bool internalLookup = g_blockEspInternalBlockLookup;
    const auto original = reinterpret_cast<BlockSourceGetBlockFn>(g_blockSourceGetBlockOriginal);
    if (!original) return nullptr;

    g_blockEspInternalBlockLookup = true;
    void* block = original(blockSource, position);
    g_blockEspInternalBlockLookup = internalLookup;

    if (internalLookup || !g_blockEspCaptureEvents.load(std::memory_order_acquire))
        return block;

    g_blockSourceGetBlockCalls.fetch_add(1u, std::memory_order_relaxed);
    if (!blockSource || blockSource != g_blockEspActiveBlockSource.load(std::memory_order_acquire) ||
        !block || reinterpret_cast<std::uintptr_t>(block) <= 0x1000u ||
        !isValidEventPosition(position)) return block;

    const std::int64_t dx = static_cast<std::int64_t>(position.x) -
        g_blockEspCaptureCenterX.load(std::memory_order_relaxed);
    const std::int64_t dy = static_cast<std::int64_t>(position.y) -
        g_blockEspCaptureCenterY.load(std::memory_order_relaxed);
    const std::int64_t dz = static_cast<std::int64_t>(position.z) -
        g_blockEspCaptureCenterZ.load(std::memory_order_relaxed);
    // Avoid spending hook/queue work on the immediate area already refreshed
    // every other tick by BlockESP's local scanner.
    if (dx * dx + dy * dy + dz * dz <= 64) return block;

    const auto* name = reinterpret_cast<const bedrocktools::sdk::Block*>(block)->fullName();
    if (!name || name->empty() || !BlockEspModule::isSupportedBlockName(*name))
        return block;

    g_blockSourceGetBlockCandidateCalls.fetch_add(1u, std::memory_order_relaxed);
    enqueueBlockEventPosition(blockSource, position, *name);
    return block;
}

void enqueueBlockEventCandidate(void* updateObject, void* source) noexcept {
    if (!updateObject || reinterpret_cast<std::uintptr_t>(updateObject) <= 0x1000u) return;

    // The Apollon generic callback passes updateObject + 8 as a three-int BlockPos.
    // Copy it immediately: the engine may destroy or recycle the event object.
    bedrocktools::sdk::BlockPos position{};
    std::memcpy(&position, static_cast<const std::uint8_t*>(updateObject) + 8u, sizeof(position));
    enqueueBlockEventPosition(source, position);
}

void enqueueApollonVectorCandidate(void* eventObject, void* source) noexcept {
    if (!eventObject || reinterpret_cast<std::uintptr_t>(eventObject) <= 0x1000u) return;

    // Apollon's spawner callbacks convert the float Vec3 at eventObject + 0x10
    // to integer block coordinates (FCVTZS), then pass those coordinates to
    // its common candidate insertion routine.
    std::array<float, 3> coordinates{};
    std::memcpy(coordinates.data(), static_cast<const std::uint8_t*>(eventObject) + 0x10u,
                sizeof(coordinates));
    for (const float coordinate : coordinates) {
        if (!std::isfinite(coordinate) || coordinate < -30000000.0f || coordinate > 30000000.0f)
            return;
    }

    const bedrocktools::sdk::BlockPos position{
        static_cast<int>(coordinates[0]),
        static_cast<int>(coordinates[1]),
        static_cast<int>(coordinates[2])};
    enqueueBlockEventPosition(source, position);
}

std::vector<BlockEventCandidate> drainBlockEventCandidates(std::size_t maximum, void* expectedSource) {
    (void)expectedSource;
    auto result = g_blockEventCandidates.drain(maximum);
    g_eventCandidatesDrained.fetch_add(static_cast<std::uint64_t>(result.size()), std::memory_order_relaxed);
    return result;
}

void blockUpdateHook(void* updateObject, void* blockSource) {
    g_genericBlockUpdateCalls.fetch_add(1u, std::memory_order_relaxed);
    // The generic event broadcaster can dispatch block events whose second
    // context argument is null. The event object's BlockPos is still valid;
    // candidates are resolved later against the active player BlockSource.
    // Do not use the optional context pointer as a capture gate.
    if (g_blockEspCaptureEvents.load(std::memory_order_acquire))
        enqueueBlockEventCandidate(updateObject, blockSource);

    // The 26.60 target is the matching two-argument event broadcaster. The
    // Apollon wrapper reads the BlockPos from argument 0 + 8 and forwards both
    // original arguments unchanged. Always forward, even while BlockESP is off.
    const auto original = reinterpret_cast<BlockUpdateFn>(g_blockUpdateOriginal);
    if (original) original(updateObject, blockSource);
}

// BedrockTools resolves this exact lookup used by chunk tessellation. Unlike a
// radius scan, it is called with the real BlockSource and BlockPos as the game
// builds geometry for a loaded chunk, including chunks far from the player.
// Read only the already-resolved block name to reject ordinary terrain; do
// not call BlockSource::getBlock or mutate the ESP cache on the render thread.
// Candidate positions are revalidated and cached on the player tick.
const void* blockSourceTessellationGetBlockHook(
    void* blockSource, const bedrocktools::sdk::BlockPos* position, std::int32_t layer) {
    g_blockSourceTessellationGetBlockCalls.fetch_add(1u, std::memory_order_relaxed);
    const auto original = reinterpret_cast<BlockSourceGetBlockForTessellationFn>(
        g_blockSourceTessellationGetBlockOriginal);
    const void* block = original ? original(blockSource, position, layer) : nullptr;
    if (g_blockEspCaptureEvents.load(std::memory_order_acquire) && blockSource && position &&
        reinterpret_cast<std::uintptr_t>(position) > 0x1000u && block &&
        reinterpret_cast<std::uintptr_t>(block) > 0x1000u && isValidEventPosition(*position)) {
        // The Block returned by this renderer lookup is already resolved. Filter
        // non-target terrain here so the bounded queue isn't flooded by stone,
        // dirt and other ordinary blocks from a freshly tessellated chunk.
        const auto* name = reinterpret_cast<const bedrocktools::sdk::Block*>(block)->fullName();
        if (name && !name->empty() && BlockEspModule::isSupportedBlockName(*name)) {
            g_blockSourceTessellationCandidateCalls.fetch_add(1u, std::memory_order_relaxed);
            // The renderer has already resolved this exact Block. Preserve its
            // canonical identifier in the queue so the player tick can classify
            // it without issuing a second BlockSource lookup.
            enqueueBlockEventPosition(blockSource, *position, *name);
        }
    }
    return block;
}

bool installBlockSourceTessellationGetBlockHook() {
    if (g_blockSourceTessellationGetBlockHook) return true;
    const auto address = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::BlockSourceGetBlockForTessellation);
    if (!address) return false;

    g_blockSourceTessellationGetBlockOriginal = nullptr;
    auto hook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(address),
        reinterpret_cast<void*>(blockSourceTessellationGetBlockHook),
        &g_blockSourceTessellationGetBlockOriginal);
    if (!hook || !g_blockSourceTessellationGetBlockOriginal) {
        if (hook) bedrocktools::hooks::remove(hook);
        g_blockSourceTessellationGetBlockOriginal = nullptr;
        return false;
    }
    g_blockSourceTessellationGetBlockHook = hook;
    return true;
}

void blockEventVectorHook0(void* eventObject, void* blockSource) {
    g_apollonEventCalls[0].fetch_add(1u, std::memory_order_relaxed);
    if (g_blockEspCaptureEvents.load(std::memory_order_acquire))
        enqueueApollonVectorCandidate(eventObject, blockSource);
    const auto original = reinterpret_cast<BlockUpdateFn>(g_apollonEventOriginals[0]);
    if (original) original(eventObject, blockSource);
}
void blockEventVectorHook1(void* eventObject, void* blockSource) {
    g_apollonEventCalls[1].fetch_add(1u, std::memory_order_relaxed);
    if (g_blockEspCaptureEvents.load(std::memory_order_acquire))
        enqueueApollonVectorCandidate(eventObject, blockSource);
    const auto original = reinterpret_cast<BlockUpdateFn>(g_apollonEventOriginals[1]);
    if (original) original(eventObject, blockSource);
}
void blockEventPositionHook0(void* eventObject, void* blockSource) {
    g_apollonEventCalls[2].fetch_add(1u, std::memory_order_relaxed);
    if (g_blockEspCaptureEvents.load(std::memory_order_acquire))
        enqueueBlockEventCandidate(eventObject, blockSource);
    const auto original = reinterpret_cast<BlockUpdateFn>(g_apollonEventOriginals[2]);
    if (original) original(eventObject, blockSource);
}
void blockEventPositionHook1(void* eventObject, void* blockSource) {
    g_apollonEventCalls[3].fetch_add(1u, std::memory_order_relaxed);
    if (g_blockEspCaptureEvents.load(std::memory_order_acquire))
        enqueueBlockEventCandidate(eventObject, blockSource);
    const auto original = reinterpret_cast<BlockUpdateFn>(g_apollonEventOriginals[3]);
    if (original) original(eventObject, blockSource);
}

// The remaining Apollon wrappers all follow the same two-argument ABI and
// differ only in where the position is stored inside the event object.
template <std::size_t Index, bool VectorCoordinates>
void apollonAdditionalBlockEventHook(void* eventObject, void* blockSource) {
    g_apollonEventCalls[Index].fetch_add(1u, std::memory_order_relaxed);
    if (g_blockEspCaptureEvents.load(std::memory_order_acquire)) {
        if constexpr (VectorCoordinates)
            enqueueApollonVectorCandidate(eventObject, blockSource);
        else
            enqueueBlockEventCandidate(eventObject, blockSource);
    }
    const auto original = reinterpret_cast<BlockUpdateFn>(g_apollonEventOriginals[Index]);
    if (original) original(eventObject, blockSource);
}

template <std::size_t ExpectedWords>
bool installApollonEventHook(std::size_t index, std::uintptr_t engineBase,
                             std::uintptr_t rva, const std::uint32_t (&expected)[ExpectedWords],
                             void* replacement) {
    if (index >= kApollonEventHookCount) return false;
    if (g_apollonEventHooks[index]) return true;

    auto* target = reinterpret_cast<std::uint8_t*>(engineBase + rva);
    // Treat legacy RVAs as candidates only. Verify the address belongs to the
    // same mapped Minecraft image before reading memory or installing a hook.
    if (!isAddressInMinecraftImage(target, engineBase)) return false;
    if (std::memcmp(target, expected, sizeof(expected)) != 0) return false;

    g_apollonEventOriginals[index] = nullptr;
    auto hook = bedrocktools::hooks::install(
        target, replacement, &g_apollonEventOriginals[index]);
    if (!hook || !g_apollonEventOriginals[index]) {
        if (hook) bedrocktools::hooks::remove(hook);
        g_apollonEventOriginals[index] = nullptr;
        return false;
    }
    g_apollonEventHooks[index] = hook;
    return true;
}

bool installApollonBlockEventHooks() {
    const auto getBlock = g_blockEspGetBlock.load(std::memory_order_acquire);
    if (!getBlock) return false;

    std::uintptr_t engineBase = 0;
    if (!getMinecraftImageBase(reinterpret_cast<const void*>(getBlock), engineBase))
        return false;

    // These RVAs and prologues were verified against the same libminecraftpe.so
    // bundled with Apollon. Each callback mirrors the matching Apollon wrapper:
    // two event types store a float Vec3 at +0x10; two store BlockPos at +0x8.
    constexpr std::uint32_t mobSpawnerPrologue[] = {
        0xA9BE7BFDu, 0xA9014FF4u, 0x910003FDu, 0xAA0003F4u
    }; // 0x106D0930
    constexpr std::uint32_t trialSpawnerPrologue[] = {
        0xA9BD7BFDu, 0xF9000BF5u, 0xA9024FF4u, 0x910003FDu
    }; // 0x1063617C
    // Minecraft 26.60 equivalent of Apollon's Vault callback target.
    // 64 exact bytes match the 26.52.3 function; the later member offsets
    // were adjusted by +8 in 26.60, so retain a full 16-instruction guard.
    constexpr std::uint32_t vaultPrologue[] = {
        0xFC170FECu, 0x6D012BEBu, 0x6D0223E9u, 0xA9037BFdu,
        0xA9046FFCu, 0xA90567FAu, 0xA9065FF8u, 0xA90757F6u,
        0xA9084FF4u, 0x9100C3FDu, 0xD10643FFu, 0xD53BD059u,
        0xAA0103F4u, 0xAA0003F3u, 0xF9401728u, 0xF81C03A8u
    }; // 26.60 RVA 0x10E6A098
    constexpr std::uint32_t furnacePrologue[] = {
        0xA9BA7BFDu, 0xF9000BFCu, 0xA90267FAu, 0xA9035FF8u
    }; // 0x106C5628

    constexpr std::uint32_t observerBlockPrologue[] = {
        0xD10203FFu, 0xA9057BFDu, 0xF90033F5u, 0xA9074FF4u
    }; // 0x1062B4CC
    constexpr std::uint32_t redstoneBlockPrologue[] = {
        0xD10603FFu, 0xFD008BEAu, 0x6D11A3E9u, 0xA912FBFDu
    }; // 0x106F96C8
    constexpr std::uint32_t utilityVectorPrologue[] = {
        0xA9BE7BFDu, 0xA9014FF4u, 0x910003FDu, 0xD1010014u
    }; // 0x106C8340
    constexpr std::uint32_t utilityCounterPrologue[] = {
        0xB9400808u, 0x39403009u, 0x11000508u, 0xB9000808u
    }; // 0x10636614; Apollon intentionally hooks this interior method entry.
    constexpr std::uint32_t utilityDispatchPrologue[] = {
        0xD101C3FFu, 0x6D0323E9u, 0xA9047BFDu, 0xA90557F6u
    }; // 0x106FF2B4
    // 26.60 storage dispatcher candidate: 48-byte sequence match against
    // Apollon's 26.52.3 target, followed by the same two-argument flow.
    constexpr std::uint32_t storageDispatchPrologue[] = {
        0xA9BB7BFDu, 0xF9000BFCu, 0xA9025FF8u, 0xA90357F6u,
        0xA9044FF4u, 0x910003FDu, 0xD10903FFu, 0xD53BD056u,
        0xAA0003F3u, 0xAA0103F4u, 0xF94016C8u, 0xF81F83A8u
    }; // 26.60 RVA 0x10F73930
    // 26.60 BlockSource dispatcher candidate: 64-byte exact sequence match.
    constexpr std::uint32_t blockSourceDispatchPrologue[] = {
        0xD101C3FFu, 0xA9027BFDu, 0xA90367FAu, 0xA9045FF8u,
        0xA90557F6u, 0xA9064FF4u, 0x910083FDu, 0xD53BD057u,
        0xAA0103F3u, 0xAA0003F4u, 0xF94016E8u, 0xF81F83A8u,
        0xF9400028u, 0x91002001u, 0xAA1303E0u, 0xF9400908u
    }; // 26.60 RVA 0x10F3B7F0
    constexpr std::uint32_t blockStateDispatchPrologue[] = {
        0xD10203FFu, 0xA9057BFDu, 0xA90657F6u, 0xA9074FF4u
    }; // 0x1072AB54

    bool installedAny = false;
    installedAny |= installApollonEventHook(0u, engineBase, 0x106D0930u,
                                            mobSpawnerPrologue,
                                            reinterpret_cast<void*>(blockEventVectorHook0));
    installedAny |= installApollonEventHook(1u, engineBase, 0x1063617Cu,
                                            trialSpawnerPrologue,
                                            reinterpret_cast<void*>(blockEventVectorHook1));
    installedAny |= installApollonEventHook(2u, engineBase, 0x10E6A098u,
                                            vaultPrologue,
                                            reinterpret_cast<void*>(blockEventPositionHook0));
    installedAny |= installApollonEventHook(3u, engineBase, 0x106C5628u,
                                            furnacePrologue,
                                            reinterpret_cast<void*>(blockEventPositionHook1));

    // Additional specialized hooks registered by Apollon's JNI_OnLoad installer.
    // Their original implementations are still called after lightweight capture.
    installedAny |= installApollonEventHook(4u, engineBase, 0x1062B4CCu,
                                            observerBlockPrologue,
                                            reinterpret_cast<void*>(apollonAdditionalBlockEventHook<4u, false>));
    installedAny |= installApollonEventHook(5u, engineBase, 0x106F96C8u,
                                            redstoneBlockPrologue,
                                            reinterpret_cast<void*>(apollonAdditionalBlockEventHook<5u, false>));
    installedAny |= installApollonEventHook(6u, engineBase, 0x106C8340u,
                                            utilityVectorPrologue,
                                            reinterpret_cast<void*>(apollonAdditionalBlockEventHook<6u, true>));
    installedAny |= installApollonEventHook(7u, engineBase, 0x10636614u,
                                            utilityCounterPrologue,
                                            reinterpret_cast<void*>(apollonAdditionalBlockEventHook<7u, true>));
    installedAny |= installApollonEventHook(8u, engineBase, 0x106FF2B4u,
                                            utilityDispatchPrologue,
                                            reinterpret_cast<void*>(apollonAdditionalBlockEventHook<8u, false>));
    installedAny |= installApollonEventHook(9u, engineBase, 0x10F73930u,
                                            storageDispatchPrologue,
                                            reinterpret_cast<void*>(apollonAdditionalBlockEventHook<9u, false>));
    installedAny |= installApollonEventHook(10u, engineBase, 0x10F3B7F0u,
                                            blockSourceDispatchPrologue,
                                            reinterpret_cast<void*>(apollonAdditionalBlockEventHook<10u, false>));
    installedAny |= installApollonEventHook(11u, engineBase, 0x1072AB54u,
                                            blockStateDispatchPrologue,
                                            reinterpret_cast<void*>(apollonAdditionalBlockEventHook<11u, false>));
    return installedAny;
}

bool installBlockUpdateHook() {
    if (g_blockUpdateHook) return true;
    const auto getBlock = g_blockEspGetBlock.load(std::memory_order_acquire);
    if (!getBlock) return false;

    std::uintptr_t engineBase = 0;
    if (!getMinecraftImageBase(reinterpret_cast<const void*>(getBlock), engineBase))
        return false;
    auto* target = reinterpret_cast<std::uint8_t*>(engineBase + kApollonBlockUpdateRva);
    if (!isAddressInMinecraftImage(target, engineBase)) return false;

    // Version guard for Minecraft 1.26.60. The target RVA was remapped by
    // matching the listener-vector dispatch flow against Apollon's original
    // 1.26.52.3 target; the function's first four ARM64 instructions are also
    // checked below before any detour is installed.
    constexpr std::uint32_t expectedPrologue[] = {
        0xD10303FFu, // sub sp, sp, #0xc0
        0xA9067BFDu, // stp x29, x30, [sp, #0x60]
        0xF9003BFBu, // str x27, [sp, #0x70]
        0xA90867FAu  // stp x26, x25, [sp, #0x80]
    };
    if (std::memcmp(target, expectedPrologue, sizeof(expectedPrologue)) != 0) return false;

    g_blockUpdateOriginal = nullptr;
    auto hook = bedrocktools::hooks::install(
        target, reinterpret_cast<void*>(blockUpdateHook), &g_blockUpdateOriginal);
    if (!hook || !g_blockUpdateOriginal) {
        if (hook) bedrocktools::hooks::remove(hook);
        g_blockUpdateOriginal = nullptr;
        return false;
    }
    g_blockUpdateHook = hook;
    return true;
}

constexpr std::size_t kMaxBlockEspCache = 32768;
constexpr std::size_t kMaxBlockEspRendered = 1024;

void resolveBlockGetBlock() {
    if (g_blockEspGetBlock.load(std::memory_order_acquire)) return;
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlock);
    if (address) g_blockEspGetBlock.store(reinterpret_cast<BlockSourceGetBlockFn>(address), std::memory_order_release);
}
bool installBlockSourceGetBlockHook() {
    if (g_blockSourceGetBlockHook) return true;
    const auto target = g_blockEspGetBlock.load(std::memory_order_acquire);
    if (!target) return false;

    g_blockSourceGetBlockOriginal = nullptr;
    auto hook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(blockSourceGetBlockHook),
        &g_blockSourceGetBlockOriginal);
    if (!hook || !g_blockSourceGetBlockOriginal) {
        if (hook) bedrocktools::hooks::remove(hook);
        g_blockSourceGetBlockOriginal = nullptr;
        return false;
    }
    g_blockSourceGetBlockHook = hook;
    return true;
}
void removeBlockSourceGetBlockHook() {
    if (g_blockSourceGetBlockHook) {
        bedrocktools::hooks::remove(g_blockSourceGetBlockHook);
        g_blockSourceGetBlockHook = nullptr;
    }
    g_blockSourceGetBlockOriginal = nullptr;
}
std::string configText(const nlohmann::json& j, const char* key, const char* fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (it->is_string()) return it->get<std::string>();
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    return it->dump();
}
bool parseBool(std::string_view value, bool& out) {
    if (value == "true" || value == "1" || value == "on") { out = true; return true; }
    if (value == "false" || value == "0" || value == "off") { out = false; return true; }
    return false;
}
bool parseInt(std::string_view value, int& out) {
    try {
        std::string s(value); std::size_t n = 0; int v = std::stoi(s, &n);
        while (n < s.size() && std::isspace(static_cast<unsigned char>(s[n]))) ++n;
        if (n != s.size()) return false; out = v; return true;
    } catch (...) { return false; }
}
bool parseFloat(std::string_view value, float& out) {
    try {
        std::string s(value); std::size_t n = 0; float v = std::stof(s, &n);
        while (n < s.size() && std::isspace(static_cast<unsigned char>(s[n]))) ++n;
        if (n != s.size() || !std::isfinite(v)) return false; out = v; return true;
    } catch (...) { return false; }
}
bool parseColor(std::string_view value, std::uint32_t& out) {
    std::string s(value); if (!s.empty() && s.front() == '#') s.erase(s.begin());
    if (s.size() != 6 && s.size() != 8) return false;
    if (!std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) return false;
    try {
        const auto n = static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
        out = s.size() == 6 ? (0xFF000000u | n) : n; return true;
    } catch (...) { return false; }
}
void loadColor(const nlohmann::json& j, const char* key, std::uint32_t& value) {
    if (!j.contains(key) || !j[key].is_string()) return;
    std::uint32_t parsed = 0;
    if (parseColor(j[key].get<std::string>(), parsed)) value = parsed;
}
std::string stripNamespace(std::string_view name) {
    const auto pos = name.find(':');
    if (pos != std::string_view::npos) name.remove_prefix(pos + 1);
    // Match classify(): legacy tile. prefixes may also be capitalized by
    // Bedrock's block-name wrappers. Strip before lowercasing so utility IDs
    // still match their individual config keys (crafting_table, piston, etc.).
    if (name.size() >= 5u &&
        (name[0] == 't' || name[0] == 'T') &&
        (name[1] == 'i' || name[1] == 'I') &&
        (name[2] == 'l' || name[2] == 'L') &&
        (name[3] == 'e' || name[3] == 'E') && name[4] == '.') name.remove_prefix(5u);
    std::string out(name);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
const std::vector<std::string>& utilityBlockNames() {
    static const std::vector<std::string> names = {
        "crafting_table","brewing_stand","anvil","chipped_anvil","damaged_anvil",
        "smithing_table","enchanting_table","loom","stonecutter","cartography_table",
        "fletching_table","grindstone","composter","lectern","beacon","bell","jukebox",
        "note_block","observer","piston","sticky_piston","crafter","redstone_block",
        "lever","target","block_button","redstone_torch","redstone_wire","redstone_lamp",
        "block_pressure_plate",
        // Additional BlockESP targets also exposed by Apollon.
        "bed","creaking_heart","shelf","copper_chest"
    };
    return names;
}
std::string utilityTitle(std::string name) {
    if (name == "block_button") return "Block Button";
    if (name == "block_pressure_plate") return "Pressure Plate";
    std::replace(name.begin(), name.end(), '_', ' ');
    bool upper = true;
    for (char& ch : name) {
        if (upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        upper = ch == ' ';
    }
    return "Show " + name;
}
}

BlockEspModule::BlockEspModule()
    : Module("BlockESP", "Colored outlines and tracers for useful blocks: shulkers, storage, redstone and workstations.") {
    m_queryDedupSlots.resize(1u << 15);
    showInMenu = true;
    for (const auto& name : utilityBlockNames()) m_utilityFilters.emplace(name, true);
}

std::size_t BlockEspModule::PositionHash::operator()(const PositionKey& p) const noexcept {
    std::uint64_t h = static_cast<std::uint32_t>(p.x);
    h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.z)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return static_cast<std::size_t>(h);
}
BlockEspModule::PositionKey BlockEspModule::keyFor(const bedrocktools::sdk::BlockPos& p) noexcept {
    return {p.x, p.y, p.z};
}
bool BlockEspModule::markQueryPosition(const PositionKey& position) noexcept {
    if (m_queryDedupSlots.empty()) return false;
    const std::size_t mask = m_queryDedupSlots.size() - 1u;
    std::size_t slotIndex = PositionHash{}(position) & mask;
    for (std::size_t probe = 0; probe < m_queryDedupSlots.size(); ++probe) {
        auto& slot = m_queryDedupSlots[slotIndex];
        if (slot.generation != m_queryGeneration) {
            slot.position = position;
            slot.generation = m_queryGeneration;
            ++m_uniqueQueriesThisTick;
            return true;
        }
        if (slot.position == position) return false;
        slotIndex = (slotIndex + 1u) & mask;
    }
    return false;
}
bool BlockEspModule::classify(std::string_view raw, Kind& kind) {
    // Engine builds expose equivalent IDs in several forms:
    // minecraft:crafting_table, tile.crafting_table, and legacy tile.shulkerBox.
    // Normalize once into a bounded stack buffer before matching. This keeps
    // the tessellation hook allocation-free and makes ID matching case-insensitive.
    const auto namespaceEnd = raw.find(':');
    if (namespaceEnd != std::string_view::npos) raw.remove_prefix(namespaceEnd + 1);
    if (raw.size() >= 5u &&
        (raw[0] == 't' || raw[0] == 'T') &&
        (raw[1] == 'i' || raw[1] == 'I') &&
        (raw[2] == 'l' || raw[2] == 'L') &&
        (raw[3] == 'e' || raw[3] == 'E') && raw[4] == '.') raw.remove_prefix(5u);
    if (raw.empty() || raw.size() >= 256u) return false;

    std::array<char, 256> normalized{};
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(raw[i]);
        normalized[i] = static_cast<char>(std::tolower(ch));
    }
    const std::string_view tileName(normalized.data(), raw.size());

    // Preserve the canonical distinctions between chest families and all
    // other target IDs; only the visual Kind is shared by related block types.
    if (tileName == "shulkerbox" || tileName == "shulker_box" ||
        tileName.find("shulker_box") != std::string_view::npos) kind = Kind::Shulker;
    else if (tileName == "ender_chest") kind = Kind::EnderChest;
    else if (tileName == "copper_chest") kind = Kind::Utility;
    else if (tileName == "chest" || tileName == "trapped_chest" ||
             tileName.find("chest") != std::string_view::npos) kind = Kind::Chest;
    else if (tileName == "vault") kind = Kind::Vault;
    else if (tileName == "mob_spawner" || tileName == "trial_spawner" ||
             tileName == "sculk_shrieker" || tileName.find("spawner") != std::string_view::npos) kind = Kind::Spawner;
    else if (tileName == "hopper") kind = Kind::Hopper;
    else if (tileName == "dispenser") kind = Kind::Dispenser;
    else if (tileName == "dropper") kind = Kind::Dropper;
    else if (tileName == "barrel") kind = Kind::Barrel;
    else if (tileName == "furnace" || tileName == "blast_furnace" ||
             tileName == "smoker") kind = Kind::Furnace;
    else if (tileName == "crafting_table" || tileName == "brewing_stand" || tileName == "anvil" ||
             tileName == "chipped_anvil" || tileName == "damaged_anvil" || tileName == "smithing_table" ||
             tileName == "enchanting_table" || tileName == "loom" || tileName == "stonecutter" ||
             tileName == "cartography_table" || tileName == "fletching_table" || tileName == "grindstone" ||
             tileName == "composter" || tileName == "lectern" || tileName == "beacon" || tileName == "bell" ||
             tileName == "jukebox" || tileName == "note_block" || tileName == "observer" ||
             tileName == "piston" || tileName == "sticky_piston" || tileName == "crafter" ||
             tileName == "redstone_block" || tileName == "redstone_torch" || tileName == "redstone_wire" ||
             tileName == "redstone_lamp" || tileName == "lever" ||
             tileName.find("button") != std::string_view::npos ||
             tileName.find("pressure_plate") != std::string_view::npos || tileName == "target" ||
             tileName == "bed" || tileName == "creaking_heart" || tileName == "shelf" ||
             tileName.starts_with("shelf_")) kind = Kind::Utility;
    else return false;
    return true;
}
bool BlockEspModule::isSupportedBlockName(std::string_view name) {
    Kind ignored{};
    return classify(name, ignored);
}
std::uint32_t BlockEspModule::defaultColor(Kind kind) noexcept {
    switch (kind) {
        case Kind::Shulker: return 0xFFFF40FFu;
        case Kind::Chest: return 0xFFFFB52Eu;
        case Kind::EnderChest: return 0xFFB45CFFu;
        case Kind::Hopper: return 0xFF55DDE0u;
        case Kind::Dispenser: return 0xFF65A5FFu;
        case Kind::Dropper: return 0xFF4E78D2u;
        case Kind::Barrel: return 0xFFB77A42u;
        case Kind::Furnace: return 0xFFFF6655u;
        case Kind::Utility: return 0xFF8DDF73u;
        default: return 0xFFFFFFFFu;
    }
}
bool BlockEspModule::isKindEnabled(Kind kind) const noexcept {
    switch (kind) {
        case Kind::Shulker: return showShulkers;
        case Kind::Chest: return showChests;
        case Kind::EnderChest: return showEnderChests;
        case Kind::Vault: return showVaults;
        case Kind::Spawner: return showSpawners;
        case Kind::Hopper: return showHoppers;
        case Kind::Dispenser: return showDispensers;
        case Kind::Dropper: return showDroppers;
        case Kind::Barrel: return showBarrels;
        case Kind::Furnace: return showFurnaces;
        case Kind::Utility: return true;
        default: return false;
    }
}
bool BlockEspModule::isBlockEnabled(const Outline& outline) const {
    if (outline.kind != Kind::Utility) return isKindEnabled(outline.kind);

    // The master utility toggle must gate every individually configurable
    // Apollon/Bedaim workstation target. Previously this setting was persisted
    // and exposed in the config but never consulted by the renderer filter.
    if (!showUtilityBlocks) return false;

    const auto it = m_utilityFilters.find(outline.blockName);
    return it != m_utilityFilters.end() && it->second;
}
std::uint32_t BlockEspModule::colorFor(Kind kind) const noexcept {
    switch (kind) {
        case Kind::Shulker: return shulkerColor;
        case Kind::Chest: return chestColor;
        case Kind::Vault: return 0xFFFFD34Eu;
        case Kind::Spawner: return 0xFF45E8D2u;
        case Kind::EnderChest: return enderChestColor;
        case Kind::Hopper: return hopperColor;
        case Kind::Dispenser: return dispenserColor;
        case Kind::Dropper: return dropperColor;
        case Kind::Barrel: return barrelColor;
        case Kind::Furnace: return furnaceColor;
        case Kind::Utility: return utilityColor;
        default: return defaultColor(kind);
    }
}
std::uint32_t BlockEspModule::colorForBlock(Kind kind, std::string_view name) const noexcept {
    if (kind != Kind::Utility) return colorFor(kind);
    if (name == "vault") return 0xFFFFD34Eu;
    if (name.find("spawner") != std::string::npos || name == "mob_spawner") return 0xFF45E8D2u;
    if (name == "sculk_shrieker") return 0xFF8A4DCEu;
    if (name == "redstone_block" || name == "redstone_torch" || name == "redstone_wire" || name == "redstone_lamp") return 0xFFFF3030u;
    if (name == "block_button" || name == "block_pressure_plate" ||
        name == "polished_blackstone_button" || name == "polished_blackstone_pressure_plate") return 0xFFB9B9B9u;
    if (name == "lever") return 0xFFFFD34Eu;
    if (name == "target") return 0xFFFF5252u;
    if (name == "beacon") return 0xFF6DF7F0u;
    if (name == "crafting_table") return 0xFFB77A42u;
    if (name == "copper_chest") return 0xFFB87333u;
    if (name == "brewing_stand") return 0xFF9A78D1u;
    if (name == "anvil" || name == "chipped_anvil" || name == "damaged_anvil") return 0xFF777F8Au;
    if (name == "smithing_table" || name == "stonecutter") return 0xFF9BA7B0u;
    if (name == "enchanting_table") return 0xFF8B4DCCu;
    if (name == "loom" || name == "cartography_table" || name == "fletching_table") return 0xFFD4A56Au;
    if (name == "grindstone") return 0xFFB9C3C9u;
    if (name == "composter") return 0xFF7C9A45u;
    if (name == "lectern") return 0xFFB98954u;
    if (name == "bell") return 0xFFFFD05Au;
    if (name == "jukebox" || name == "note_block") return 0xFF9A633Au;
    if (name == "observer" || name == "piston" || name == "sticky_piston" || name == "crafter") return 0xFF8E9A9Fu;
    if (name == "bed") return 0xFF5FA8E8u;
    if (name == "creaking_heart") return 0xFFB06B52u;
    if (name == "shelf" || name.starts_with("shelf_")) return 0xFFD5A76Bu;
    if (name == "copper_chest") return 0xFFB87333u;
    return utilityColor;
}
void BlockEspModule::clearCache() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_cache.clear();
    m_cacheRefreshPositions.clear();
    m_cacheRefreshCursor = 0;
}
void BlockEspModule::onInit() {
    // Keep the version-resolved BlockSource lookup for the bounded tick scanner.
    // Experimental Apollon-derived detours stay disabled until their 26.60 ABI
    // and callback layouts are validated against the exact runtime binary.
    resolveBlockGetBlock();
    if (kEnableExperimentalNativeEventHooks) {
        installBlockUpdateHook();
        installApollonBlockEventHooks();
        installBlockSourceTessellationGetBlockHook();
    }
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { onPlayerTick(event.player); });
    // Player-driven block changes are observable before the next chunk
    // tessellation pass. Queue their coordinates immediately, then let the
    // normal player-tick path resolve the live BlockSource and update the cache.
    bedrocktools::events::bus().subscribe<bedrocktools::events::GameModeActionEvent>(
        [this](auto& event) {
            if (!g_blockEspCaptureEvents.load(std::memory_order_acquire) ||
                !event.hasBlockPosition) return;
            using Action = bedrocktools::events::GameModeAction;
            switch (event.action) {
                case Action::StartDestroyBlock:
                case Action::StopDestroyBlock:
                case Action::StartBuildBlock:
                case Action::UseItemOn:
                    enqueueBlockEventPosition(nullptr, event.blockPosition);
                    break;
                default:
                    break;
            }
        });
}
void BlockEspModule::onEnable() {
    g_blockEspActiveBlockSource.store(nullptr, std::memory_order_release);
    g_blockEspCaptureEvents.store(true, std::memory_order_release);
    resolveBlockGetBlock();
    if (kEnableExperimentalNativeEventHooks) {
        installBlockSourceGetBlockHook();
    }
    clearBlockEventCandidates();
    if (auto* hitbox = ModuleRegistry::get().find("bedrocktools.Hitbox"))
        static_cast<HitboxModule*>(hitbox)->ensureRenderHook();
    m_blockSource = nullptr; m_scanActive = false; m_hasCenter = false; m_scanCursor = 0; m_scanTotal = 0;
    m_priorityScanActive = false; m_hasPriorityCenter = false; m_priorityDirectionDirty = false; m_priorityScanCursor = 0;
    m_localScanActive = false; m_hasLocalScanOrigin = false; m_localScanCursor = 0;
    m_hasPreviousPlayerPosition = false; m_hasMovementDirection = false;
    clearCache();
}
void BlockEspModule::onDisable() {
    g_blockEspCaptureEvents.store(false, std::memory_order_release);
    g_blockEspActiveBlockSource.store(nullptr, std::memory_order_release);
    removeBlockSourceGetBlockHook();
    clearBlockEventCandidates();
    m_scanActive = false; m_hasCenter = false; m_scanCursor = 0; m_scanTotal = 0;
    m_priorityScanActive = false; m_hasPriorityCenter = false; m_priorityDirectionDirty = false; m_priorityScanCursor = 0;
    m_hasPreviousPlayerPosition = false; m_hasMovementDirection = false;
    clearCache();
}
void BlockEspModule::onPlayerTick(bedrocktools::sdk::Player* player) {
    ++m_tickCount;
    if (!enabled || !player) return;

    auto* dimension = player->dimension();
    auto* source = dimension ? dimension->blockSource() : nullptr;
    if (!source) return;
    g_blockEspActiveBlockSource.store(source, std::memory_order_release);
    if (source != m_blockSource) {
        m_blockSource = source;
        m_scanActive = false; m_hasCenter = false; m_scanCursor = 0; m_scanTotal = 0;
        m_priorityScanActive = false; m_hasPriorityCenter = false; m_priorityDirectionDirty = false; m_priorityScanCursor = 0;
        m_localScanActive = false; m_hasLocalScanOrigin = false; m_localScanCursor = 0;
        m_hasPreviousPlayerPosition = false; m_hasMovementDirection = false;
        clearCache();
        clearBlockEventCandidates();
    }
    if (!g_blockEspGetBlock.load(std::memory_order_acquire) &&
        m_tickCount - m_lastResolveAttemptTick >= 40) {
        m_lastResolveAttemptTick = m_tickCount;
        resolveBlockGetBlock();
    }
    if (kEnableExperimentalNativeEventHooks &&
        m_tickCount - m_lastResolveAttemptTick >= 40) {
        m_lastResolveAttemptTick = m_tickCount;
        // Retry only when the experimental 26.60 ABI-guarded path is explicitly
        // re-enabled after runtime validation.
        installBlockSourceTessellationGetBlockHook();
        installBlockUpdateHook();
        installApollonBlockEventHooks();
        installBlockSourceGetBlockHook();
    }
    const auto getBlock = g_blockEspGetBlock.load(std::memory_order_acquire);
    if (!getBlock) return;

    // Keep the native event path observable before removing the fallback scanner.
    // The counters distinguish installed hooks from callbacks that actually fire
    // in the current world; a later event-only migration must pass this check.
    if ((m_tickCount % 200u) == 0u) {
        char state[768]{};
        const auto genericCalls = g_genericBlockUpdateCalls.load(std::memory_order_relaxed);
        std::size_t installedMask = 0u;
        std::size_t offset = static_cast<std::size_t>(std::snprintf(
            state, sizeof(state),
            "block_get_hook=%s;block_get_calls=%llu;block_get_capture=%llu;generic_hook=%s;generic=%llu;tess_lookup=%llu;tess_capture=%llu;queued=%llu;merged=%llu;drained=%llu;matched=%llu;dropped=%llu;named_drained=%llu;dedup_bypassed=%llu;classify_rejected=%llu;event_outlines=%llu;installed=",
            g_blockSourceGetBlockHook ? "installed" : "missing",
            static_cast<unsigned long long>(g_blockSourceGetBlockCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_blockSourceGetBlockCandidateCalls.load(std::memory_order_relaxed)),
            g_blockUpdateHook ? "installed" : "missing",
            static_cast<unsigned long long>(genericCalls),
            static_cast<unsigned long long>(g_blockSourceTessellationGetBlockCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_blockSourceTessellationCandidateCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesQueued.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesMerged.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesDrained.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesClassified.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesDropped.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesNamedDrained.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesDedupBypassed.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesClassifyRejected.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesOutlines.load(std::memory_order_relaxed))));
        for (std::size_t i = 0; i < kApollonEventHookCount && offset < sizeof(state); ++i) {
            if (g_apollonEventHooks[i]) installedMask |= (std::size_t{1} << i);
            const int written = std::snprintf(state + offset, sizeof(state) - offset,
                "%s%llu", i ? "," : "", static_cast<unsigned long long>(
                    g_apollonEventCalls[i].load(std::memory_order_relaxed)));
            if (written <= 0) break;
            offset