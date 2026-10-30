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
    if (name.starts_with("tile.")) name.remove_prefix(5);
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
    // Keep Apollon's legacy identifier convention explicit: every supported
    // target is matched as tile.<block_name>. Modern BedrockTools may expose
    // minecraft:<block_name> or the bare name, so normalize those representations
    // to the same tile suffix without allocating in the scan hot path.
    const auto namespaceEnd = raw.find(':');
    if (namespaceEnd != std::string_view::npos) raw.remove_prefix(namespaceEnd + 1);
    if (raw.starts_with("tile.")) raw.remove_prefix(5);
    const std::string_view tileName = raw;

    // Apollon's fast storage targets: tile.chest, tile.trapped_chest,
    // tile.ender_chest, tile.shulkerBox / tile.shulker_box and tile.barrel.
    if (tileName == "shulkerBox" || tileName == "shulkerbox" || tileName == "shulker_box" ||
        tileName.find("shulker_box") != std::string_view::npos) kind = Kind::Shulker;
    else if (tileName == "ender_chest") kind = Kind::EnderChest;
    else if (tileName == "copper_chest") kind = Kind::Utility;
    else if (tileName == "chest" || tileName == "trapped_chest" ||
             tileName.find("chest") != std::string_view::npos) kind = Kind::Chest;
    else if (tileName == "vault") kind = Kind::Vault;
    // All Apollon spawner families are explicit tile IDs.
    else if (tileName == "mob_spawner" || tileName == "trial_spawner" ||
             tileName == "sculk_shrieker" || tileName.find("spawner") != std::string_view::npos) kind = Kind::Spawner;
    else if (tileName == "hopper") kind = Kind::Hopper;
    else if (tileName == "dispenser") kind = Kind::Dispenser;
    else if (tileName == "dropper") kind = Kind::Dropper;
    else if (tileName == "barrel") kind = Kind::Barrel;
    // The three furnace identifiers used by Apollon.
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
    // EXPERIMENTAL: restore the native capture installation used by Apollon v6.7
    // in PR #123. The original RVAs/prologue guards are intentionally retained
    // for this first compatibility test; a 26.60 mismatch must fail closed.
    // Keep the 26.60 tessellation hook and player-tick validation as fallbacks.
    resolveBlockGetBlock();
    installBlockUpdateHook();
    installApollonBlockEventHooks();
    installBlockSourceTessellationGetBlockHook();
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
    g_blockEspCaptureEvents.store(true, std::memory_order_release);
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
    if (source != m_blockSource) {
        m_blockSource = source;
        m_scanActive = false; m_hasCenter = false; m_scanCursor = 0; m_scanTotal = 0;
        m_priorityScanActive = false; m_hasPriorityCenter = false; m_priorityDirectionDirty = false; m_priorityScanCursor = 0;
        m_localScanActive = false; m_hasLocalScanOrigin = false; m_localScanCursor = 0;
        m_hasPreviousPlayerPosition = false; m_hasMovementDirection = false;
        clearCache();
        clearBlockEventCandidates();
    }
    const bool nativeHooksMissing = !g_blockUpdateHook;
    if ((!g_blockEspGetBlock.load(std::memory_order_acquire) ||
         !g_blockSourceTessellationGetBlockHook || nativeHooksMissing) &&
        m_tickCount - m_lastResolveAttemptTick >= 40) {
        m_lastResolveAttemptTick = m_tickCount;
        resolveBlockGetBlock();
        installBlockSourceTessellationGetBlockHook();
        // Signature resolution may finish after module initialization. Retry
        // native hooks on the same bounded cadence instead of permanently
        // missing the event path when the first onInit attempt runs too early.
        installBlockUpdateHook();
        installApollonBlockEventHooks();
    }
    const auto getBlock = g_blockEspGetBlock.load(std::memory_order_acquire);
    if (!getBlock) return;

    // Keep the native event path observable before removing the fallback scanner.
    // The counters distinguish installed hooks from callbacks that actually fire
    // in the current world; a later event-only migration must pass this check.
    if ((m_tickCount % 200u) == 0u) {
        char state[512]{};
        const auto genericCalls = g_genericBlockUpdateCalls.load(std::memory_order_relaxed);
        std::size_t installedMask = 0u;
        std::size_t offset = static_cast<std::size_t>(std::snprintf(
            state, sizeof(state),
            "generic_hook=%s;generic=%llu;tess_lookup=%llu;tess_capture=%llu;queued=%llu;merged=%llu;drained=%llu;matched=%llu;dropped=%llu;installed=",
            g_blockUpdateHook ? "installed" : "missing",
            static_cast<unsigned long long>(genericCalls),
            static_cast<unsigned long long>(g_blockSourceTessellationGetBlockCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_blockSourceTessellationCandidateCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesQueued.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesMerged.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesDrained.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesClassified.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_eventCandidatesDropped.load(std::memory_order_relaxed))));
        for (std::size_t i = 0; i < kApollonEventHookCount && offset < sizeof(state); ++i) {
            if (g_apollonEventHooks[i]) installedMask |= (std::size_t{1} << i);
            const int written = std::snprintf(state + offset, sizeof(state) - offset,
                "%s%llu", i ? "," : "", static_cast<unsigned long long>(
                    g_apollonEventCalls[i].load(std::memory_order_relaxed)));
            if (written <= 0) break;
            offset += static_cast<std::size_t>(written);
        }
        char detail[128]{};
        std::snprintf(detail, sizeof(detail),
            "block_update_hook=%s; tessellation_lookup_hook=%s; specialized_hook_mask=0x%03llX; counts=cumulative",
            g_blockUpdateHook ? "installed" : "missing",
            g_blockSourceTessellationGetBlockHook ? "installed" : "missing",
            static_cast<unsigned long long>(installedMask));
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
            "BlockESP", "native_event_activity", state, detail);
    }

    const auto p = player->position();
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return;
    const bedrocktools::sdk::BlockPos current{
        static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z))};

    // Infer horizontal movement from successive positions instead of relying on a
    // version-specific velocity accessor. This also works with Elytra and knockback.
    bool headingChanged = false;
    float horizontalMovement = 0.0f;
    if (m_hasPreviousPlayerPosition) {
        const float moveX = p.x - m_previousPlayerPosition.x;
        const float moveZ = p.z - m_previousPlayerPosition.z;
        horizontalMovement = std::hypot(moveX, moveZ);
        if (horizontalMovement > 0.5f) {
            const float nextX = moveX / horizontalMovement;
            const float nextZ = moveZ / horizontalMovement;
            if (m_hasMovementDirection) {
                const float directionDot = nextX * m_moveDirectionX + nextZ * m_moveDirectionZ;
                headingChanged = directionDot < 0.80f;
            }
            m_moveDirectionX = nextX;
            m_moveDirectionZ = nextZ;
            m_hasMovementDirection = true;
        }
    }
    m_previousPlayerPosition = p;
    m_hasPreviousPlayerPosition = true;
    if (headingChanged) m_priorityDirectionDirty = true;

    const int rx = maxScanRadius ? 320 : std::clamp(horizontalRadius, 20, 320);
    const int ry = maxScanRadius ? 160 : std::clamp(verticalRadius, 10, 160);
    const std::size_t scanBudget = static_cast<std::size_t>(std::clamp(scanBudgetPerTick, 256, 16384));

    // Cull cached blocks outside the active scan/render radius, with a small
    // margin to avoid flicker while moving around the boundary.
    if (m_tickCount % 10u == 0u) {
        const int keepHorizontal = rx + 8;
        const int keepVertical = ry + 8;
        const int keepHorizontalSquared = keepHorizontal * keepHorizontal;
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            const std::int64_t dx = static_cast<std::int64_t>(it->second.position.x) - current.x;
            const std::int64_t dy = static_cast<std::int64_t>(it->second.position.y) - current.y;
            const std::int64_t dz = static_cast<std::int64_t>(it->second.position.z) - current.z;
            if (dx * dx + dz * dz > static_cast<std::int64_t>(keepHorizontalSquared) || std::abs(dy) > keepVertical)
                it = m_cache.erase(it);
            else
                ++it;
        }
    }

    // Each query contributes to a batched cache update. The old implementation
    // acquired the cache mutex for every queried block (often >16,000 locks/tick).
    // Batch erases/inserts once per tick. If BlockSource cannot resolve a queried
    // position, evict any old outline there so unloaded chunks do not leave stale ESP.
    std::vector<PositionKey> scannedPositions;
    std::vector<Outline> foundOutlines;
    scannedPositions.reserve(scanBudget + 729u);
    foundOutlines.reserve(64u);
    // Several scan passes overlap by design. Deduplicate their coordinates before
    // calling the native BlockSource function; a native lookup is the expensive part.
    if (++m_queryGeneration == 0u) {
        for (auto& slot : m_queryDedupSlots) slot.generation = 0u;
        m_queryGeneration = 1u;
    }
    m_uniqueQueriesThisTick = 0u;
    auto inspectBlock = [&](const bedrocktools::sdk::BlockPos& pos, bool eventCandidate = false,
                            std::string_view resolvedName = {}) -> bool {
        const PositionKey key = keyFor(pos);
        if (!markQueryPosition(key)) return false;
        // A successful scan invalidates the old outline first; a currently valid
        // target block is reinserted below. This also removes stale cached blocks
        // when the native lookup says the chunk/position is no longer available.
        scannedPositions.push_back(key);
        std::string_view blockName = resolvedName;
        if (blockName.empty()) {
            void* raw = getBlock(source, pos);
            if (!raw || reinterpret_cast<std::uintptr_t>(raw) <= 0x1000) return true;
            const auto* name = reinterpret_cast<const bedrocktools::sdk::Block*>(raw)->fullName();
            if (!name || name->empty()) return true;
            blockName = *name;
        }

        Kind kind{};
        if (!classify(blockName, kind)) return true;
        if (eventCandidate) g_eventCandidatesClassified.fetch_add(1u, std::memory_order_relaxed);

        // Do not require adjacent air/water for utility targets. That extra
        // six-neighbour query made distant workstations (notably crafting tables)
        // disappear whenever neighbouring chunk lookups were unavailable. The
        // classifier and per-block filters are the sole inclusion criteria.

        std::string normalizedName = stripNamespace(blockName);
        if (normalizedName.size() >= 7 && normalizedName.ends_with("_button")) normalizedName = "block_button";
        if (normalizedName.ends_with("_pressure_plate")) normalizedName = "block_pressure_plate";
        if (normalizedName.starts_with("shelf_")) normalizedName = "shelf";
        foundOutlines.push_back(Outline{pos, colorForBlock(kind, normalizedName), kind, std::move(normalizedName)});
        return true;
    };

    // Revalidate a rotating slice of already-discovered blocks every tick.
    // This catches distant blocks that were broken after the broad scan passed
    // their coordinates, without forcing a full 320-block scan to restart.
    const std::size_t cacheValidationBudget = std::min<std::size_t>(256u, scanBudget / 64u);
    std::vector<PositionKey> cachedValidationPositions;
    if (cacheValidationBudget > 0u) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        // Snapshot cache keys once per validation sweep. The unordered_map order
        // may change as records are refreshed, so walking it by index each tick
        // could repeatedly revisit the same subset and starve older records.
        if (m_cacheRefreshCursor >= m_cacheRefreshPositions.size()) {
            m_cacheRefreshPositions.clear();
            m_cacheRefreshPositions.reserve(m_cache.size());
            for (const auto& [key, outline] : m_cache) {
                (void)outline;
                m_cacheRefreshPositions.push_back(key);
            }
            m_cacheRefreshCursor = 0u;
        }
        const std::size_t remaining = m_cacheRefreshPositions.size() - m_cacheRefreshCursor;
        const std::size_t count = std::min(cacheValidationBudget, remaining);
        cachedValidationPositions.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const PositionKey key = m_cacheRefreshPositions[m_cacheRefreshCursor + i];
            if (m_cache.contains(key)) cachedValidationPositions.push_back(key);
        }
        m_cacheRefreshCursor += count;
    }
    for (const auto& key : cachedValidationPositions) {
        inspectBlock(bedrocktools::sdk::BlockPos{key.x, key.y, key.z});
    }

    // Very fast near-player refresh: immediate changes/removals are checked every
    // other tick, while the much larger local volume below is scanned incrementally.
    constexpr int kLocalRefreshRadius = 4;
    if ((m_tickCount & 1u) == 0u) {
        for (int dx = -kLocalRefreshRadius; dx <= kLocalRefreshRadius; ++dx) {
            for (int dy = -kLocalRefreshRadius; dy <= kLocalRefreshRadius; ++dy) {
                for (int dz = -kLocalRefreshRadius; dz <= kLocalRefreshRadius; ++dz) {
                    inspectBlock(bedrocktools::sdk::BlockPos{current.x + dx, current.y + dy, current.z + dz});
                }
            }
        }
    }

    // Incremental local scan: 64x160x64 (655,360 positions). Unlike the
    // previous x/z-column-major order, scan complete horizontal slices first:
    // same-height chests across the nearby chunks are now discovered immediately,
    // while vertical coverage continues center-out in subsequent slices.
    constexpr int kLocalHalfWidth = 32;
    constexpr int kLocalHalfHeight = 80;
    constexpr std::size_t kLocalVolumeSize = 64u * 160u * 64u;
    bool localMovedFar = false;
    if (m_hasLocalScanOrigin) {
        const int dx = current.x - m_localScanOrigin.x;
        const int dy = current.y - m_localScanOrigin.y;
        const int dz = current.z - m_localScanOrigin.z;
        localMovedFar = dx * dx + dz * dz >= 64 || std::abs(dy) >= 16;
    }
    // Revisit the local volume periodically even when the player stands still.
    // Otherwise blocks placed/broken after the first pass can remain undiscovered
    // until movement or a manual scan-setting change happens to restart scanning.
    constexpr std::uint64_t kLocalRescanIntervalTicks = 40u;
    const bool localRescanDue = (m_tickCount % kLocalRescanIntervalTicks) == 0u;
    if (!m_localScanActive && (!m_hasLocalScanOrigin || localMovedFar || localRescanDue)) {
        m_localScanOrigin = current;
        m_localScanCursor = 0;
        m_localScanOffsets.clear();
        m_localScanOffsets.reserve(kLocalVolumeSize);

        auto centerOutOffset = [](int index) -> int {
            if (index == 0) return 0;
            const int magnitude = (index + 1) / 2;
            return (index & 1) ? -magnitude : magnitude;
        };
        std::vector<std::pair<int, int>> localHorizontal;
        localHorizontal.reserve(32u * 32u);
        for (int dx = -kLocalHalfWidth; dx < kLocalHalfWidth; ++dx) {
            for (int dz = -kLocalHalfWidth; dz < kLocalHalfWidth; ++dz)
                localHorizontal.emplace_back(dx, dz);
        }
        // Finish the nearest chunk before spending the scan budget on farther
        // chunks. The old per-block rings interleaved chunks and delayed a full
        // chunk's worth of chests until many ticks later.
        auto floorChunk = [](int block) -> int {
            return block >= 0 ? block / 16 : (block - 15) / 16;
        };
        const int playerChunkX = floorChunk(m_localScanOrigin.x);
        const int playerChunkZ = floorChunk(m_localScanOrigin.z);
        std::sort(localHorizontal.begin(), localHorizontal.end(),
            [&](const auto& a, const auto& b) {
                const int ax = floorChunk(m_localScanOrigin.x + a.first) - playerChunkX;
                const int az = floorChunk(m_localScanOrigin.z + a.second) - playerChunkZ;
                const int bx = floorChunk(m_localScanOrigin.x + b.first) - playerChunkX;
                const int bz = floorChunk(m_localScanOrigin.z + b.second) - playerChunkZ;
                const int aChunkDistance = ax * ax + az * az;
                const int bChunkDistance = bx * bx + bz * bz;
                if (aChunkDistance != bChunkDistance) return aChunkDistance < bChunkDistance;
                // Stable chunk-coordinate tie-break groups every column of a
                // chunk together instead of interleaving equally distant chunks.
                if (ax != bx) return ax < bx;
                if (az != bz) return az < bz;
                const int aBlockDistance = a.first * a.first + a.second * a.second;
                const int bBlockDistance = b.first * b.first + b.second * b.second;
                return aBlockDistance < bBlockDistance;
            });
        std::vector<int> localVertical;
        localVertical.reserve(160u);
        for (int i = 0; i < 160; ++i) localVertical.push_back(centerOutOffset(i));
        for (const int dy : localVertical) {
            for (const auto& [dx, dz] : localHorizontal) {
                m_localScanOffsets.push_back({dx, dy, dz});
            }
        }
        m_localScanActive = !m_localScanOffsets.empty();
        m_hasLocalScanOrigin = true;
    }

    // The priority scan is the fast discovery path near the player. While moving,
    // it becomes a forward corridor. While stationary, it scans a center-out
    // 17x17x17 cube. Keep this bounded: expanding this volume caused extra lag
    // without fixing the underlying chunk-discovery limitation.
    bool movedFromPriorityCenter = false;
    if (m_hasPriorityCenter) {
        const int dx = current.x - m_priorityScanOrigin.x;
        const int dy = current.y - m_priorityScanOrigin.y;
        const int dz = current.z - m_priorityScanOrigin.z;
        movedFromPriorityCenter = dx * dx + dz * dz >= 25 || std::abs(dy) >= 5;
    }
    // The priority volume must also cycle while stationary, so a new target
    // just outside the immediate 9x9x9 refresh is found without user interaction.
    constexpr std::uint64_t kPriorityRescanIntervalTicks = 20u;
    const bool priorityRescanDue = (m_tickCount % kPriorityRescanIntervalTicks) == 0u;
    if (!m_priorityScanActive &&
        (!m_hasPriorityCenter || movedFromPriorityCenter || m_priorityDirectionDirty || priorityRescanDue)) {
        m_priorityScanOrigin = current;
        m_priorityScanCursor = 0;
        m_priorityScanOffsets.clear();
        m_priorityScanOffsets.reserve(4913u);

        auto centerOutOffset = [](int index) -> int {
            if (index == 0) return 0;
            const int magnitude = (index + 1) / 2;
            return (index & 1) ? -magnitude : magnitude;
        };
        if (horizontalMovement > 0.5f && m_hasMovementDirection) {
            for (int forward = 0; forward <= 24; ++forward) {
                for (int lateralIndex = 0; lateralIndex <= 10; ++lateralIndex) {
                    const int lateral = centerOutOffset(lateralIndex);
                    for (int verticalIndex = 0; verticalIndex <= 16; ++verticalIndex) {
                        const int dy = centerOutOffset(verticalIndex);
                        const int dx = static_cast<int>(std::lround(
                            static_cast<float>(forward) * m_moveDirectionX -
                            static_cast<float>(lateral) * m_moveDirectionZ));
                        const int dz = static_cast<int>(std::lround(
                            static_cast<float>(forward) * m_moveDirectionZ +
                            static_cast<float>(lateral) * m_moveDirectionX));
                        m_priorityScanOffsets.push_back({dx, dy, dz});
                    }
                }
            }
        } else {
            for (int shell = 0; shell <= 8; ++shell) {
                for (int dx = -shell; dx <= shell; ++dx) {
                    for (int dy = -shell; dy <= shell; ++dy) {
                        for (int dz = -shell; dz <= shell; ++dz) {
                            if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != shell) continue;
                            m_priorityScanOffsets.push_back({dx, dy, dz});
                        }
                    }
                }
            }
        }
        m_priorityScanActive = !m_priorityScanOffsets.empty();
        m_hasPriorityCenter = true;
        m_priorityDirectionDirty = false;
    }

    // Start/recenter the wide background scan only when no wide pass is running.
    // Its cursor therefore keeps progressing during flight instead of restarting
    // every time the player crosses another block or chunk.
    constexpr int moveThreshold = 32;
    const bool movedFar = std::abs(current.x - m_lastCenter.x) >= moveThreshold ||
        std::abs(current.z - m_lastCenter.z) >= moveThreshold ||
        std::abs(current.y - m_lastCenter.y) >= moveThreshold;
    // Restart a completed pass even if the player has not moved. New blocks
    // and removals must eventually be noticed without changing a menu setting.
    const bool widePassComplete = m_scanTotal > 0u && m_scanCursor >= m_scanTotal;
    if (!m_hasCenter || (!m_scanActive && (movedFar || widePassComplete))) {
        m_scanCenter = current;
        m_lastCenter = current;
        m_hasCenter = true;
        m_scanCursor = 0;

        const std::size_t diameter = static_cast<std::size_t>(rx * 2 + 1);
        m_horizontalScanOffsets.clear();
        m_horizontalScanOffsets.reserve(diameter * diameter);
        m_horizontalScanOffsets.emplace_back(0, 0);
        // Precompute center-out rings once per background pass.
        for (int ring = 1; ring <= rx; ++ring) {
            for (int dx = -ring; dx <= ring; ++dx) {
                m_horizontalScanOffsets.emplace_back(dx, -ring);
                m_horizontalScanOffsets.emplace_back(dx, ring);
            }
            for (int dz = -ring + 1; dz < ring; ++dz) {
                m_horizontalScanOffsets.emplace_back(-ring, dz);
                m_horizontalScanOffsets.emplace_back(ring, dz);
            }
        }
        // Repeat the player's Y layer after each pair of adjacent vertical
        // layers: 0, -1, +1, 0, -2, +2, ... . This keeps distant, same-height
        // blocks fresh instead of revisiting the player's height only once per
        // enormous 161-layer pass.
        m_scanTotal = m_horizontalScanOffsets.size() * static_cast<std::size_t>(ry * 3);
        m_scanActive = true;
    }

    // Event observations are serviced first, but validated on this tick using
    // the current BlockSource. This avoids invoking engine getters/classifiers
    // from inside the hooked update method.
    const std::size_t eventBudget = std::min<std::size_t>(2048u, scanBudget / 2u);
    const auto eventCandidates = drainBlockEventCandidates(eventBudget, source);
    for (const auto& candidate : eventCandidates) {
        const std::string_view resolvedName = candidate.blockNameLength
            ? std::string_view(candidate.blockName.data(), candidate.blockNameLength)
            : std::string_view{};
        const bedrocktools::sdk::BlockPos candidatePosition{
            candidate.position.x, candidate.position.y, candidate.position.z};
        inspectBlock(candidatePosition, true, resolvedName);
    }

    // Reserve half the budget for the same-height-first local sweep. Priority
    // and wide scans consume only the unique-query budget that remains after
    // immediate refreshes, cache validation and event candidates.
    const std::size_t localBudget = std::min<std::size_t>(
        scanBudget / 2u, scanBudget > m_uniqueQueriesThisTick ? scanBudget - m_uniqueQueriesThisTick : 0u);
    if (m_localScanActive) {
        const std::size_t end = std::min(m_localScanOffsets.size(), m_localScanCursor + localBudget);
        for (std::size_t index = m_localScanCursor; index < end; ++index) {
            const auto& offset = m_localScanOffsets[index];
            inspectBlock(bedrocktools::sdk::BlockPos{
                m_localScanOrigin.x + offset.x,
                m_localScanOrigin.y + offset.y,
                m_localScanOrigin.z + offset.z});
        }
        m_localScanCursor = end;
        if (m_localScanCursor >= m_localScanOffsets.size()) m_localScanActive = false;
    }
    const std::size_t priorityBudget = std::min<std::size_t>(
        scanBudget / 8u, scanBudget > m_uniqueQueriesThisTick ? scanBudget - m_uniqueQueriesThisTick : 0u);
    if (m_priorityScanActive) {
        const std::size_t end = std::min(m_priorityScanOffsets.size(), m_priorityScanCursor + priorityBudget);
        for (std::size_t index = m_priorityScanCursor; index < end; ++index) {
            const auto& offset = m_priorityScanOffsets[index];
            inspectBlock(bedrocktools::sdk::BlockPos{
                m_priorityScanOrigin.x + offset.x,
                m_priorityScanOrigin.y + offset.y,
                m_priorityScanOrigin.z + offset.z});
        }
        m_priorityScanCursor = end;
        if (m_priorityScanCursor >= m_priorityScanOffsets.size()) m_priorityScanActive = false;
    }

    // Budget against unique native queries, not cursor advances. If the local
    // and priority passes overlap, the wide pass uses the saved work to continue
    // until the per-tick query budget is actually spent.
    const std::size_t wideBudget = scanBudget > m_uniqueQueriesThisTick
        ? scanBudget - m_uniqueQueriesThisTick : 0u;
    if (m_scanActive && wideBudget > 0u) {
        const std::size_t verticalCount = static_cast<std::size_t>(ry * 3);
        const std::size_t horizontalCount = m_horizontalScanOffsets.size();
        std::size_t wideUsed = 0;
        // Scan the full horizontal plane at the player's Y, adjacent planes, and
        // revisit the player's Y every three layers: 0, -1, +1, 0, -2, +2, ...
        // Combined with center-out horizontal offsets, this quickly detects far
        // blocks at normal terrain height without abandoning vertical coverage.
        while (m_scanCursor < m_scanTotal && wideUsed < wideBudget) {
            const std::size_t index = m_scanCursor++;
            const std::size_t layer = index / horizontalCount;
            const std::size_t horizontalIndex = index % horizontalCount;
            const auto [dx, dz] = m_horizontalScanOffsets[horizontalIndex];
            const std::size_t phase = layer % 3u;
            const int dy = phase == 0u ? 0
                : phase == 1u ? -static_cast<int>((layer + 2u) / 3u)
                              : static_cast<int>((layer + 1u) / 3u);
            if (inspectBlock(bedrocktools::sdk::BlockPos{
                    m_scanCenter.x + dx, m_scanCenter.y + dy, m_scanCenter.z + dz}))
                ++wideUsed;
        }
        if (m_scanCursor >= m_scanTotal) m_scanActive = false;
    }

    if (!scannedPositions.empty() || !foundOutlines.empty()) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        for (const auto& key : scannedPositions) m_cache.erase(key);
        // insert_or_assign updates recently scanned blocks. Do not discard new
        // outlines simply because the cache has reached its nominal capacity.
        for (auto& outline : foundOutlines)
            m_cache.insert_or_assign(keyFor(outline.position), std::move(outline));

        // Keep a small slack to avoid pruning every tick, then retain the nearest
        // blocks to the player. This prevents stale/far entries from starving
        // discoveries around a newly entered area.
        constexpr std::size_t kCachePruneSlack = 512u;
        if (m_cache.size() > kMaxBlockEspCache + kCachePruneSlack) {
            using CacheIterator = decltype(m_cache.begin());
            std::vector<CacheIterator> ranked;
            ranked.reserve(m_cache.size());
            for (auto it = m_cache.begin(); it != m_cache.end(); ++it) ranked.push_back(it);

            auto distanceSquared = [&](const Outline& outline) {
                const double dx = static_cast<double>(outline.position.x) + 0.5 - p.x;
                const double dy = static_cast<double>(outline.position.y) + 0.5 - p.y;
                const double dz = static_cast<double>(outline.position.z) + 0.5 - p.z;
                return dx * dx + dy * dy + dz * dz;
            };
            std::nth_element(ranked.begin(), ranked.begin() + kMaxBlockEspCache, ranked.end(),
                [&](CacheIterator a, CacheIterator b) {
                    return distanceSquared(a->second) < distanceSquared(b->second);
                });

            std::unordered_set<PositionKey, PositionHash> retained;
            retained.reserve(kMaxBlockEspCache * 2u);
            for (std::size_t i = 0; i < kMaxBlockEspCache; ++i) retained.emplace(ranked[i]->first);
            for (auto it = m_cache.begin(); it != m_cache.end();) {
                if (!retained.contains(it->first)) it = m_cache.erase(it);
                else ++it;
            }
        }
    }
}
void BlockEspModule::getRenderSnapshot(const bedrocktools::sdk::Vec3& playerPosition,
                                       std::vector<Outline>& output) const {
    output.clear();
    if (!enabled || !masterEnabled || !keybindActive) return;
    // Rendering is independent from the active discovery radius: once a block is
    // discovered, keep drawing it from cache while it remains in the bounded cache.
    output.reserve(kMaxBlockEspRendered);

    struct Candidate {
        float distanceSquared = 0.0f;
        const Outline* outline = nullptr;
    };
    std::vector<Candidate> nearest;
    nearest.reserve(kMaxBlockEspRendered);
    // Heap top is the farthest retained block. This keeps selection O(N log K)
    // and ensures new nearby blocks are not hidden by arbitrary hash-map order.
    auto fartherHeap = [](const Candidate& a, const Candidate& b) {
        return a.distanceSquared < b.distanceSquared;
    };

    std::lock_guard<std::mutex> lock(m_cacheMutex);
    for (const auto& [key, outline] : m_cache) {
        (void)key;
        if (!isBlockEnabled(outline)) continue;
        const float dx = static_cast<float>(outline.position.x) + 0.5f - playerPosition.x;
        const float dy = static_cast<float>(outline.position.y) + 0.5f - playerPosition.y;
        const float dz = static_cast<float>(outline.position.z) + 0.5f - playerPosition.z;
        const float horizontalDistanceSquared = dx * dx + dz * dz;
        const Candidate candidate{horizontalDistanceSquared + dy * dy, &outline};
        if (nearest.size() < kMaxBlockEspRendered) {
            nearest.push_back(candidate);
            std::push_heap(nearest.begin(), nearest.end(), fartherHeap);
        } else if (candidate.distanceSquared < nearest.front().distanceSquared) {
            std::pop_heap(nearest.begin(), nearest.end(), fartherHeap);
            nearest.back() = candidate;
            std::push_heap(nearest.begin(), nearest.end(), fartherHeap);
        }
    }

    std::sort(nearest.begin(), nearest.end(), [](const Candidate& a, const Candidate& b) {
        return a.distanceSquared < b.distanceSquared;
    });
    for (const auto& candidate : nearest) {
        const auto& outline = *candidate.outline;
        output.push_back(Outline{
            outline.position, colorForBlock(outline.kind, outline.blockName), outline.kind, outline.blockName});
    }
}
void BlockEspModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema; nlohmann::json config; saveConfig(config);
    schema.defaultCategory("types")
        .category("types", "Block categories", "Toggle which useful block types are detected.")
        .category("tracer", "Tracer and outlines", "World-space outlines and lines pointing to detected blocks.")
        .category("scan", "Detection range", "Block scans are incremental to limit frame and tick spikes.")
        .category("colors", "Block colors", "HEX colors in #AARRGGBB or #RRGGBB format.");
    auto toggle = [&](const char* key, const char* title, const char* desc, bool def, const char* cat) {
        ConfigNodeV2 n; n.id=key; n.key=key; n.title=title; n.description=desc; n.category=cat;
        n.type=ConfigControlTypeV2::Toggle; n.defaultValue=def?"true":"false";
        n.currentValue=configText(config,key,n.defaultValue.c_str()); n.hasCurrentValue=true; schema.node(std::move(n));
    };
    toggle("showShulkers","Shulker boxes","All colored shulker boxes.",true,"types");
    toggle("showChests","Chests","Normal and trapped chests.",true,"types");
    toggle("showEnderChests","Ender chests","Ender chests.",true,"types");
    toggle("showVaults","Vaults","Vault blocks, including trial vaults.",true,"types");
    toggle("showSpawners","Spawners","Mob spawners, trial spawners and sculk shriekers.",true,"types");
    toggle("showHoppers","Hoppers","Hoppers.",true,"types");
    toggle("showDispensers","Dispensers","Dispensers.",true,"types");
    toggle("showDroppers","Droppers","Droppers.",true,"types");
    toggle("showBarrels","Barrels","Barrels.",true,"types");
    toggle("showFurnaces","Furnaces","Furnaces, blast furnaces and smokers.",true,"types");
    toggle("showUtilityBlocks","Utility blocks","Master switch for workstations and other individually configurable utility targets.",true,"types");
    for (const auto& name : utilityBlockNames()) {
        const std::string key = "showBlock_" + name;
        const std::string title = utilityTitle(name);
        toggle(key.c_str(), title.c_str(), "Independently detect this block type.", true, "types");
    }
    toggle("showTracer","Show tracer","Draw a line from the camera to each detected block.",true,"tracer");
    auto slider = [&](const char* key,const char* title,const char* desc,const char* cat,int def,int min,int max,int step,const char* unit) {
        ConfigNodeV2 n; n.id=key; n.key=key; n.title=title; n.description=desc; n.category=cat;
        n.type=ConfigControlTypeV2::SliderInt; n.defaultValue=std::to_string(def);
        n.currentValue=configText(config,key,n.defaultValue.c_str()); n.hasCurrentValue=true;
        n.minValue=std::to_string(min); n.maxValue=std::to_string(max); n.step=std::to_string(step); n.unit=unit; schema.node(std::move(n));
    };
    slider("horizontalRadius","Horizontal radius","Experimental long-range scan and render radius.", "scan",320,20,320,1," blocks");
    slider("verticalRadius","Vertical radius","Vertical scan and render radius for deep underground blocks.", "scan",80,10,160,1," blocks");
    toggle("maxScanRadius","Max Scan Radius","Use maximum supported radius (320 horizontal, 160 vertical blocks).",false,"scan");
    slider("scanBudgetPerTick","Scan budget","Block queries per tick; higher values load faster but use more CPU.", "scan",4096,256,8192,256," blocks/tick");
    slider("outlineThickness","Outline thickness","Approximate thickness of block outlines.", "tracer",4,1,10,1," units");
    slider("tracerThickness","Tracer thickness","World-space thickness of guide lines pointing to each block.", "tracer",4,1,10,1," units");
    auto color = [&](const char* key,const char* title,std::uint32_t def) {
        ConfigNodeV2 n; n.id=key; n.key=key; n.title=title; n.description="HEX color: #AARRGGBB (or #RRGGBB).";
        n.category="colors"; n.type=ConfigControlTypeV2::Text; n.defaultValue="";
        n.currentValue=configText(config,key,""); n.hasCurrentValue=true; n.placeholder="#AARRGGBB"; n.maxLength=9; schema.node(std::move(n));
        (void)def;
    };
    color("shulkerColor","Shulker box color",shulkerColor);
    color("chestColor","Chest color",chestColor);
    color("enderChestColor","Ender chest color",enderChestColor);
    color("hopperColor","Hopper color",hopperColor);
    color("dispenserColor","Dispenser color",dispenserColor);
    color("dropperColor","Dropper color",dropperColor);
    color("barrelColor","Barrel color",barrelColor);
    color("furnaceColor","Furnace color",furnaceColor);
    color("utilityColor","Utility block color",utilityColor);
    setConfigSchemaJson(moduleId,schema.toJson());
}
bool BlockEspModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    bool b=false;
    constexpr std::string_view utilityPrefix = "showBlock_";
    if (key.size() > utilityPrefix.size() && key.substr(0, utilityPrefix.size()) == utilityPrefix) {
        if (!parseBool(value, b)) return false;
        const std::string blockName(key.substr(utilityPrefix.size()));
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            auto it = m_utilityFilters.find(blockName);
            if (it == m_utilityFilters.end()) return false;
            it->second = b;
        }
        onMenuRegistered();
        return true;
    }
    if (key=="showTracer"||key=="showShulkers"||key=="showChests"||key=="showEnderChests"||key=="showVaults"||key=="showSpawners"||
        key=="showHoppers"||key=="showDispensers"||key=="showDroppers"||key=="showBarrels"||
        key=="showFurnaces"||key=="showUtilityBlocks") {
        if (!parseBool(value,b)) return false;
        if(key=="showTracer")showTracer=b; else if(key=="showShulkers")showShulkers=b;
        else if(key=="showChests")showChests=b; else if(key=="showEnderChests")showEnderChests=b;
        else if(key=="showVaults")showVaults=b; else if(key=="showSpawners")showSpawners=b;
        else if(key=="showHoppers")showHoppers=b; else if(key=="showDispensers")showDispensers=b;
        else if(key=="showDroppers")showDroppers=b; else if(key=="showBarrels")showBarrels=b;
        else if(key=="showFurnaces")showFurnaces=b; else showUtilityBlocks=b;
        onMenuRegistered(); return true;
    }
    int i=0;
    if(key=="maxScanRadius") { if(!parseBool(value,b)) return false; maxScanRadius=b; m_scanActive=false; m_hasCenter=false; onMenuRegistered(); return true; }
    if(key=="horizontalRadius"||key=="verticalRadius"||key=="scanBudgetPerTick"||key=="outlineThickness"||key=="tracerThickness") {
        if(!parseInt(value,i)) return false;
        const bool scanShapeChanged = key=="horizontalRadius" || key=="verticalRadius";
        if(key=="horizontalRadius")horizontalRadius=std::clamp(i,20,320);
        else if(key=="verticalRadius")verticalRadius=std::clamp(i,10,160);
        else if(key=="scanBudgetPerTick")scanBudgetPerTick=std::clamp(i,256,8192);
        else if(key=="outlineThickness")outlineThickness=static_cast<float>(std::clamp(i,1,10))/100.0f;
        else tracerThickness=static_cast<float>(std::clamp(i,1,10))/100.0f;
        // A budget change affects work per tick, not the scan region/cursor.
        // Restarting here used to make tapping Scan Budget appear to "wake up"
        // the ESP by restarting its scan from the player's position.
        if (scanShapeChanged) { m_scanActive=false; m_hasCenter=false; }
        onMenuRegistered(); return true;
    }
    std::uint32_t c=0; if(!parseColor(value,c)) return false;
    if(key=="shulkerColor")shulkerColor=c; else if(key=="chestColor")chestColor=c;
    else if(key=="enderChestColor")enderChestColor=c; else if(key=="hopperColor")hopperColor=c;
    else if(key=="dispenserColor")dispenserColor=c; else if(key=="dropperColor")dropperColor=c;
    else if(key=="barrelColor")barrelColor=c; else if(key=="furnaceColor")furnaceColor=c;
    else if(key=="utilityColor")utilityColor=c;
    else return false;
    onMenuRegistered(); return true;
}
void BlockEspModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    showTracer=j.value("showTracer",showTracer); showShulkers=j.value("showShulkers",showShulkers);
    showChests=j.value("showChests",showChests); showEnderChests=j.value("showEnderChests",showEnderChests); showVaults=j.value("showVaults",showVaults); showSpawners=j.value("showSpawners",showSpawners);
    showHoppers=j.value("showHoppers",showHoppers); showDispensers=j.value("showDispensers",showDispensers);
    showDroppers=j.value("showDroppers",showDroppers); showBarrels=j.value("showBarrels",showBarrels);
    showFurnaces=j.value("showFurnaces",showFurnaces); showUtilityBlocks=j.value("showUtilityBlocks",showUtilityBlocks);
    maxScanRadius=j.value("maxScanRadius",maxScanRadius);
    for (const auto& name : utilityBlockNames()) {
        const std::string key = "showBlock_" + name;
        auto it = m_utilityFilters.find(name);
        if (it != m_utilityFilters.end()) it->second = j.value(key, showUtilityBlocks);
    }
    horizontalRadius=std::clamp(j.value("horizontalRadius",horizontalRadius),20,320);
    verticalRadius=std::clamp(j.value("verticalRadius",verticalRadius),10,160);
    scanBudgetPerTick=std::clamp(j.value("scanBudgetPerTick",scanBudgetPerTick),256,8192);
    outlineThickness=static_cast<float>(std::clamp(j.value("outlineThickness",4),1,10))/100.0f;
    tracerThickness=static_cast<float>(std::clamp(j.value("tracerThickness",4),1,10))/100.0f;
    loadColor(j,"shulkerColor",shulkerColor); loadColor(j,"chestColor",chestColor);
    loadColor(j,"enderChestColor",enderChestColor); loadColor(j,"hopperColor",hopperColor);
    loadColor(j,"dispenserColor",dispenserColor); loadColor(j,"dropperColor",dropperColor);
    loadColor(j,"barrelColor",barrelColor); loadColor(j,"furnaceColor",furnaceColor);
    loadColor(j,"utilityColor",utilityColor);
}
void BlockEspModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["showTracer"]=showTracer; j["showShulkers"]=showShulkers; j["showChests"]=showChests;
    j["showEnderChests"]=showEnderChests; j["showVaults"]=showVaults; j["showSpawners"]=showSpawners; j["showHoppers"]=showHoppers; j["showDispensers"]=showDispensers;
    j["showDroppers"]=showDroppers; j["showBarrels"]=showBarrels; j["showFurnaces"]=showFurnaces;
    j["showUtilityBlocks"]=showUtilityBlocks; j["maxScanRadius"]=maxScanRadius;
    for (const auto& [name, enabled] : m_utilityFilters) j["showBlock_" + name] = enabled;
    j["horizontalRadius"]=horizontalRadius;
    j["verticalRadius"]=verticalRadius; j["scanBudgetPerTick"]=scanBudgetPerTick;
    j["outlineThickness"]=static_cast<int>(std::lround(outlineThickness*100.0f));
    j["tracerThickness"]=static_cast<int>(std::lround(tracerThickness*100.0f));
    auto save=[&](const char* key,std::uint32_t c){char text[12];std::snprintf(text,sizeof(text),"#%08X",c);j[key]=std::string(text);};
    save("shulkerColor",shulkerColor); save("chestColor",chestColor); save("enderChestColor",enderChestColor);
    save("hopperColor",hopperColor); save("dispenserColor",dispenserColor); save("dropperColor",dropperColor);
    save("barrelColor",barrelColor); save("furnaceColor",furnaceColor); save("utilityColor",utilityColor);
}
