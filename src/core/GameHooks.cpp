#include "GameHooks.hpp"
#include "VerifiedLocalPlayerTickEvent.hpp"
#include "Diagnostics.hpp"
#include "modules/player/PlayerGameType.hpp"
#include <bedrocktools/Version.hpp>

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/AttackResultEvent.hpp>
#include <bedrocktools/events/VerifiedLocalPlayerPreTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/offsets/Core.hpp>
#include <bedrocktools/sdk/offsets/UI.hpp>
#include <bedrocktools/sdk/offsets/World.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <cstdio>
#include <limits>
#include <iterator>
#include <pl/Logger.hpp>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bedrocktools::core::gamehooks {
namespace {
using namespace bedrocktools::events;
using bedrocktools::memory::SignatureId;

struct InteractionResultValue {
    std::uint8_t value;
};

using VersionStringFn = std::string(*)(void*);
using NormalTickFn = void(*)(void*);
using StartDestroyBlockFn = bool(*)(void*, const void*, std::uint8_t, bool*);
using StopDestroyBlockFn = void(*)(void*, const void*);
using StartBuildBlockFn = void(*)(void*, const void*, std::uint8_t);
using UseItemFn = bool(*)(void*, void*);
using UseItemAsAttackFn = bool(*)(void*, void*, const void*);
using UseItemOnFn = InteractionResultValue(*)(void*, void*, const void*, std::uint8_t, const void*, const void*, bool);
using InteractFn = bool(*)(void*, void*, const void*);
using AttackFn = bool(*)(void*, void*, bool, const void*);
using PublicAttackFn = bool(*)(void*, void*, const void*);
using ClientInstanceUpdateFn = bool(*)(void*, bool);
using ContainerSlotSelectedFn = std::uint32_t(*)(void*, const std::string&, int);
using ScreenFn = void*(*)(void*, void*, void*, void*, void*, void*, void*, void*);
using EglSwapBuffersFn = EGLBoolean(*)(EGLDisplay, EGLSurface);

VersionStringFn versionOriginal = nullptr;
NormalTickFn tickOriginal = nullptr;
StartDestroyBlockFn gameModeStartDestroyBlockOriginal = nullptr;
StartDestroyBlockFn survivalModeStartDestroyBlockOriginal = nullptr;
StopDestroyBlockFn gameModeStopDestroyBlockOriginal = nullptr;
StartBuildBlockFn gameModeStartBuildBlockOriginal = nullptr;
StartBuildBlockFn survivalModeStartBuildBlockOriginal = nullptr;
UseItemFn gameModeUseItemOriginal = nullptr;
UseItemFn survivalModeUseItemOriginal = nullptr;
UseItemAsAttackFn gameModeUseItemAsAttackOriginal = nullptr;
UseItemAsAttackFn survivalModeUseItemAsAttackOriginal = nullptr;
UseItemOnFn gameModeUseItemOnOriginal = nullptr;
UseItemOnFn survivalModeUseItemOnOriginal = nullptr;
InteractFn gameModeInteractOriginal = nullptr;
InteractFn survivalModeInteractOriginal = nullptr;
AttackFn gameModeAttackOriginal = nullptr;
ClientInstanceUpdateFn clientUpdateOriginal = nullptr;
ContainerSlotSelectedFn containerSlotSelectedOriginal = nullptr;
ScreenFn containerOpenOriginal = nullptr;
ScreenFn containerCloseOriginal = nullptr;
ScreenFn chatOpenOriginal = nullptr;
ScreenFn chatCloseOriginal = nullptr;
EglSwapBuffersFn swapBuffersOriginal = nullptr;
std::atomic<void*> currentClientInstance = nullptr;
std::atomic<void*> currentLocalPlayer = nullptr;
std::atomic<void*> currentGameMode = nullptr;
std::atomic<void*> g_localPlayerResolveClient{nullptr};
std::atomic<std::int64_t> g_nextLocalPlayerResolveNs{0};
bool shouldResolveLocalPlayer(void* client) {
 if (!client) return false;
 const void* old = g_localPlayerResolveClient.exchange(client, std::memory_order_acq_rel);
 if (old != client) g_nextLocalPlayerResolveNs.store(0, std::memory_order_release);
 // Retry quicker during world/player initialization, while keeping a hard bound on work in ClientInstance::update.
 const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
 auto due = g_nextLocalPlayerResolveNs.load(std::memory_order_acquire);
 for (;;) { if (now < due) return false; if (g_nextLocalPlayerResolveNs.compare_exchange_weak(due, now + 50000000, std::memory_order_acq_rel, std::memory_order_acquire)) return true; }
}
std::array<bedrocktools::hooks::Handle, 26> handles{};
std::size_t handleCount = 0;
std::mutex installMutex;
std::mutex attackObserverMutex;
std::atomic<void*> attackObserverTarget{nullptr};
bedrocktools::hooks::Handle attackObserverHandle = nullptr;
bool installed = false;
std::string gameVersion;
thread_local std::uint32_t gameModeActionDepth = 0;
thread_local bool syntheticAttackDispatchActive = false;
thread_local bool syntheticAttackResultObserved = false;
thread_local bool syntheticAttackNativeResult = false;
thread_local bool syntheticAttackEventsPrepublished = false;
using PlayerGetGameTypeFn = std::int32_t (*)(const void*);
using PlayerSetGameTypeFn = void (*)(void*, std::int32_t);
PlayerGetGameTypeFn playerGetGameTypeOriginal = nullptr;
PlayerSetGameTypeFn playerSetGameTypeOriginal = nullptr;
std::chrono::steady_clock::time_point playerGameTypeNextResolve{};
std::chrono::steady_clock::time_point playerSetGameTypeNextResolve{};
std::atomic_bool playerSetGameTypeHookInstalled{false};
std::atomic_bool playerSetGameTypeSeen{false};
std::mutex playerGameTypeCacheMutex;
std::unordered_map<const void*, std::int32_t> playerGameTypeCache;
void* resolveLocalPlayerFromClientInstance(void* clientInstance);


template <class Function>
bool hookSignature(SignatureId id, void* detour, Function** original) {
    const auto address = bedrocktools::memory::resolve(id);
    if (!address) return false;
    bedrocktools::hooks::Handle handle = bedrocktools::hooks::install(reinterpret_cast<void*>(address), detour, reinterpret_cast<void**>(original));
    if (!handle) return false;
    if (handleCount < handles.size()) handles[handleCount++] = handle;
    return true;
}

class GameModeActionScope {
public:
    GameModeActionScope(GameModeType modeKind, void* gameMode)
        : mModeKind(modeKind), mGameMode(gameMode), mOuter(gameModeActionDepth++ == 0) {
        if (gameMode) currentGameMode.store(gameMode, std::memory_order_release);
    }

    ~GameModeActionScope() {
        --gameModeActionDepth;
    }

    void publish(GameModeAction action, bool hasResult = false, bool result = false) const {
        if (!mOuter) return;
        GameModeActionEvent event{mModeKind, action, mGameMode, hasResult, result};
        bus().publish(event);
    }

private:
    GameModeType mModeKind;
    void* mGameMode;
    bool mOuter;
};

bool interactionSucceeded(InteractionResultValue result) {
    return (result.value & 1u) != 0;
}

bool interactionSwings(InteractionResultValue result) {
    return (result.value & 2u) != 0;
}

bool itemStackHasItem(const void* itemStack) {
    if (!itemStack) return false;
    const auto* bytes = reinterpret_cast<const std::byte*>(itemStack);
    auto* counter = *reinterpret_cast<void* const*>(bytes + bedrocktools::sdk::offsets::ShulkerPreview::ItemStackBaseItem);
    if (!counter) return false;
    return *reinterpret_cast<void* const*>(counter) != nullptr;
}

class DiagnosticGameHooksLogger {
public:
    template <typename... Args>
    void info(std::string_view format, Args&&... args) const {
        mirror(bedrocktools::diagnostics::Level::Info, format, args...);
        native().info(format, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void debug(std::string_view format, Args&&... args) const {
        mirror(bedrocktools::diagnostics::Level::Debug, format, args...);
        native().debug(format, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void warn(std::string_view format, Args&&... args) const {
        mirror(bedrocktools::diagnostics::Level::Warning, format, args...);
        native().warn(format, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void error(std::string_view format, Args&&... args) const {
        mirror(bedrocktools::diagnostics::Level::Error, format, args...);
        native().error(format, std::forward<Args>(args)...);
    }
private:
    static pl::log::Logger& native() {
        static auto& logger = pl::log::Logger::getOrCreate("Bedaim-GameHooks");
        return logger;
    }
    template <typename... Args>
    static void mirror(bedrocktools::diagnostics::Level level, std::string_view format, Args&&... args) {
        try {
            const auto message = fmt::vformat(fmt::string_view(format.data(), format.size()),
                                             fmt::make_format_args(args...));
            bedrocktools::diagnostics::log(level, "GameHooks", message);
        } catch (...) {
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error,
                "GameHooks", "failed to format native logger message");
        }
    }
};

DiagnosticGameHooksLogger& gameHooksLogger() {
    static DiagnosticGameHooksLogger logger;
    return logger;
}

// Check a pointer against the process's readable mappings before dereferencing
// memory obtained from a version-sensitive native object layout.
struct ReadableRegion {
    std::uintptr_t begin{};
    std::uintptr_t end{};
};

bool isReadableMemory(const void* pointer, std::size_t size) {
    if (!pointer || size == 0) return false;
    auto begin = reinterpret_cast<std::uintptr_t>(pointer);
#if defined(__aarch64__)
    // Android ARM64 may tag heap pointers in the top byte (for example,
    // 0xB4xxxxxxxxxxxxxx). CPU dereferences can accept tagged pointers with TBI,
    // but /proc/self/maps reports the untagged virtual address. Normalize only
    // the address used for map-range validation; keep the original pointer for
    // actual memory access so pointer-tagging semantics are preserved.
    begin &= 0x00FFFFFFFFFFFFFFULL;
#endif
    if (size > std::numeric_limits<std::uintptr_t>::max() - begin) return false;
    const auto end = begin + size;

    // GameMode validation can happen on every attack attempt. A thread-local
    // 50 ms readable-map snapshot makes those checks O(log mappings), instead of
    // opening/parsing /proc/self/maps on every call. No pointer is considered
    // valid solely because it was validated earlier; address containment is
    // still checked against the current short-lived snapshot for every query.
    thread_local std::vector<ReadableRegion> readableRegions;
    thread_local std::chrono::steady_clock::time_point mapsRefreshed{};
    thread_local bool mapsCacheValid = false;
    const auto now = std::chrono::steady_clock::now();

    if (!mapsCacheValid || now < mapsRefreshed ||
        now - mapsRefreshed > std::chrono::milliseconds(50)) {
        std::vector<ReadableRegion> refreshed;
        std::FILE* maps = std::fopen("/proc/self/maps", "r");
        if (!maps) {
            readableRegions.clear();
            mapsCacheValid = false;
            return false;
        }
        char line[512]{};
        while (std::fgets(line, sizeof(line), maps)) {
            unsigned long long regionBegin = 0, regionEnd = 0;
            char permissions[5]{};
            if (std::sscanf(line, "%llx-%llx %4s", &regionBegin, &regionEnd, permissions) == 3 &&
                permissions[0] == 'r' && regionEnd > regionBegin) {
                refreshed.push_back({static_cast<std::uintptr_t>(regionBegin),
                                     static_cast<std::uintptr_t>(regionEnd)});
            }
        }
        std::fclose(maps);
        if (refreshed.empty()) {
            readableRegions.clear();
            mapsCacheValid = false;
            return false;
        }
        readableRegions.swap(refreshed);
        mapsRefreshed = now;
        mapsCacheValid = true;
    }

    const auto it = std::upper_bound(
        readableRegions.begin(), readableRegions.end(), begin,
        [](std::uintptr_t address, const ReadableRegion& region) {
            return address < region.begin;
        });
    if (it == readableRegions.begin()) return false;
    const auto& region = *std::prev(it);
    return begin >= region.begin && end <= region.end;
}

bool isMinecraftFunction(const void* address) {
    if (!address) return false;
    Dl_info info{};
    return dladdr(address, &info) != 0 && info.dli_fname &&
        std::string_view(info.dli_fname).find("libminecraftpe.so") != std::string_view::npos;
}

// Validate that a candidate really looks like a live ClientInstance before
// caching it. Signature matches alone only identify code, not the receiver ABI.
// ClientInstance's known interface extends through getLocalPlayer (vtable slot 32).
bool isPlausibleClientInstance(void* object, std::size_t* trustedEntryCount = nullptr) {
    if (trustedEntryCount) *trustedEntryCount = 0;
    if (!object || !isReadableMemory(object, sizeof(void*))) return false;

    void** vtable = nullptr;
    std::memcpy(&vtable, object, sizeof(vtable));
    if (!vtable || !isReadableMemory(vtable, sizeof(void*))) return false;

    // ClientInstance::update is a hot path. Its receiver and vtable are stable
    // for the lifetime of a world, so do the expensive module-range scan only
    // when either identity changes. The object and its first vtable pointer are
    // still checked on every call; a changed pointer forces full revalidation.
    static std::atomic<void*> cachedObject{nullptr};
    static std::atomic<void*> cachedVtable{nullptr};
    static std::atomic<std::size_t> cachedTrustedEntries{0};
    static std::atomic<int> cachedValidation{-1};
    if (cachedObject.load(std::memory_order_acquire) == object &&
        cachedVtable.load(std::memory_order_acquire) == vtable) {
        const int validation = cachedValidation.load(std::memory_order_acquire);
        if (validation >= 0) {
            if (trustedEntryCount)
                *trustedEntryCount = cachedTrustedEntries.load(std::memory_order_relaxed);
            return validation == 1;
        }
    }

    constexpr std::size_t lastRequiredSlot =
        bedrocktools::sdk::offsets::VTable::ClientInstance_getLocalPlayer;
    if (!isReadableMemory(vtable, sizeof(void*) * (lastRequiredSlot + 1))) return false;

    std::size_t trustedEntries = 0;
    for (std::size_t i = 0; i <= lastRequiredSlot; ++i) {
        std::uintptr_t entry = 0;
        std::memcpy(&entry, vtable + i, sizeof(entry));
        if (entry && isMinecraftFunction(reinterpret_cast<const void*>(entry)))
            ++trustedEntries;
    }
    const bool valid = trustedEntries >= 2;
    cachedVtable.store(vtable, std::memory_order_relaxed);
    cachedTrustedEntries.store(trustedEntries, std::memory_order_relaxed);
    cachedValidation.store(valid ? 1 : 0, std::memory_order_relaxed);
    cachedObject.store(object, std::memory_order_release);
    if (trustedEntryCount) *trustedEntryCount = trustedEntries;
    return valid;
}

// Vtables can contain small ARM64 branch veneers instead of the final method
// address. Compare both the entry and a bounded, validated B-branch target so
// exact signature validation also works when the compiler/linker emits a thunk.
// This never follows BL/calls or reads beyond a readable instruction.
std::uintptr_t canonicalArm64Entry(std::uintptr_t address) {
#if defined(__aarch64__)
    for (int depth = 0; depth < 4 && address != 0; ++depth) {
        if (!isMinecraftFunction(reinterpret_cast<const void*>(address)) ||
            !isReadableMemory(reinterpret_cast<const void*>(address), sizeof(std::uint32_t)))
            break;
        const auto* instructionPtr = reinterpret_cast<const std::uint32_t*>(address);
        std::uint32_t instruction = 0;
        std::memcpy(&instruction, instructionPtr, sizeof(instruction));
        // Skip common landing/pointer-authentication instructions emitted before a veneer.
        if (instruction == 0xD503245Fu || instruction == 0xD503201Fu ||
            instruction == 0xD503233Fu || instruction == 0xD50323Bfu) {
            address += sizeof(std::uint32_t);
            continue;
        }
        // AArch64 B imm26 (not BL). Sign-extend the 26-bit word offset.
        if ((instruction & 0xFC000000u) != 0x14000000u) break;
        std::int64_t imm26 = static_cast<std::int64_t>(instruction & 0x03FFFFFFu);
        if ((imm26 & (1LL << 25)) != 0) imm26 |= ~((1LL << 26) - 1);
        const auto next = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(address) + (imm26 * 4));
        if (!next || !isMinecraftFunction(reinterpret_cast<const void*>(next))) break;
        address = next;
    }
#endif
    return address;
}

void* resolveMinecraftFunction(const char* name) {
    if (!name || !*name) return nullptr;
    void* address = dlsym(RTLD_DEFAULT, name);
    if (isMinecraftFunction(address)) return address;
    void* library = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (!library) return nullptr;
    address = dlsym(library, name);
    const bool valid = isMinecraftFunction(address);
    dlclose(library);
    return valid ? address : nullptr;
}

void* resolveLoadedClientSupportFunction(const char* name) {
    if (!name || !*name) return nullptr;
    void* address = dlsym(RTLD_DEFAULT, name);
    if (!address) return nullptr;

    Dl_info info{};
    if (!dladdr(address, &info) || !info.dli_fname) return nullptr;
    const std::string_view libraryPath(info.dli_fname);
    // closeContainerScreen is a client-SDK helper on some distributions, not
    // necessarily a symbol in libminecraftpe.so. Accept it only from a known
    // loaded Bedrock/LeviLamina runtime image, never from an arbitrary module.
    const bool trustedImage =
        libraryPath.find("libminecraftpe.so") != std::string_view::npos ||
        libraryPath.find("liblevilamina.so") != std::string_view::npos ||
        libraryPath.find("libLeviLamina.so") != std::string_view::npos;
    return trustedImage ? address : nullptr;
}

void playerSetGameTypeDetour(void* player, std::int32_t nativeValue) {
    if (playerSetGameTypeOriginal) playerSetGameTypeOriginal(player, nativeValue);
    if (!player) return;

    playerSetGameTypeSeen.store(true, std::memory_order_release);
    std::int32_t canonical = -1;
    std::lock_guard<std::mutex> cacheLock(playerGameTypeCacheMutex);
    if (bedaim::playergametype::normalizeNativeValue(nativeValue, canonical)) {
        playerGameTypeCache[player] = canonical;
    } else {
        playerGameTypeCache.erase(player);
    }
}

bool installPlayerSetGameTypeHook() {
    if (playerSetGameTypeHookInstalled.load(std::memory_order_acquire)) return true;
    // Player Radar needs mode updates for remote Player instances. Do not
    // mistake a LocalPlayer-only setter for a complete remote-player source.
    const char* symbols[] = {
        "_ZN6Player17setPlayerGameTypeE8GameType",
        "_ZN6Player18$setPlayerGameTypeE8GameType"
    };
    for (const char* symbol : symbols) {
        void* address = resolveMinecraftFunction(symbol);
        if (!address) address = resolveLoadedClientSupportFunction(symbol);
        if (!address || handleCount >= handles.size()) continue;
        auto handle = bedrocktools::hooks::install(
            address,
            reinterpret_cast<void*>(playerSetGameTypeDetour),
            reinterpret_cast<void**>(&playerSetGameTypeOriginal));
        if (!handle) {
            playerSetGameTypeOriginal = nullptr;
            continue;
        }
        handles[handleCount++] = handle;
        playerSetGameTypeHookInstalled.store(true, std::memory_order_release);
        gameHooksLogger().info(
            "Installed a trusted Player::setPlayerGameType observer; remote player modes can be cached when the engine updates them.");
        return true;
    }
    return false;
}

void resolvePlayerGameTypeApi() {
    std::lock_guard<std::mutex> apiLock(playerGameTypeCacheMutex);
    const auto resolveNow = std::chrono::steady_clock::now();
    if (!playerGetGameTypeOriginal && resolveNow >= playerGameTypeNextResolve) {
        playerGameTypeNextResolve = resolveNow + std::chrono::seconds(1);
        playerGetGameTypeOriginal = reinterpret_cast<PlayerGetGameTypeFn>(
            resolveMinecraftFunction("_ZNK6Player17getPlayerGameTypeEv"));
        if (!playerGetGameTypeOriginal) {
            playerGetGameTypeOriginal = reinterpret_cast<PlayerGetGameTypeFn>(
                resolveMinecraftFunction("_ZNK11LocalPlayer17getPlayerGameTypeEv"));
        }
    }
    if (!playerSetGameTypeHookInstalled.load(std::memory_order_acquire) &&
        resolveNow >= playerSetGameTypeNextResolve) {
        playerSetGameTypeNextResolve = resolveNow + std::chrono::seconds(1);
        installPlayerSetGameTypeHook();
    }

}

// A public virtual entry may be a guarded wrapper rather than a one-instruction
// ARM64 veneer. Accept it only when its first 16 instructions contain a direct
// unconditional B to the independently resolved, unique target-build signature.
// This validates the wrapper-to-method relationship without guessing a vtable slot
// or calling the scanned signature address directly.
bool boundedArm64BranchReaches(std::uintptr_t entry, std::uintptr_t target) {
#if defined(__aarch64__)
    if (!entry || !target ||
        !isMinecraftFunction(reinterpret_cast<const void*>(entry)) ||
        !isMinecraftFunction(reinterpret_cast<const void*>(target))) return false;
    // Some builds insert additional guard/telemetry instructions before a
    // wrapper's tail branch. Follow a bounded window, but only accept an
    // unconditional B whose destination is exactly the independently resolved
    // target; BL/calls are deliberately not followed.
    for (std::size_t i = 0; i < 48; ++i) {
        const std::uintptr_t instructionAddress = entry + i * sizeof(std::uint32_t);
        if (!isReadableMemory(reinterpret_cast<const void*>(instructionAddress), sizeof(std::uint32_t)))
            return false;
        std::uint32_t instruction = 0;
        std::memcpy(&instruction, reinterpret_cast<const void*>(instructionAddress), sizeof(instruction));
        // AArch64 B (not BL): op[31:26] == 000101, imm26 is signed in words.
        if ((instruction & 0xFC000000u) != 0x14000000u) continue;
        std::int64_t immediate = static_cast<std::int64_t>(instruction & 0x03FFFFFFu);
        if (immediate & 0x02000000LL) immediate -= 0x04000000LL;
        const auto branchTarget = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(instructionAddress) + immediate * 4);
        if (branchTarget == target) return true;
    }
#else
    (void)entry;
    (void)target;
#endif
    return false;
}

bool plausibleGameMode(void* gameMode) {
    if (!gameMode || !isReadableMemory(gameMode, sizeof(void*))) return false;
    void** vtable = nullptr;
    std::memcpy(&vtable, gameMode, sizeof(vtable));
    if (!vtable || !isReadableMemory(vtable, 8 * sizeof(void*))) return false;
    void* first = nullptr;
    std::memcpy(&first, vtable, sizeof(first));
    return first && isMinecraftFunction(first);
}

// Recognize the public three-argument GameMode::attack adapter. It forwards
// x2 (Vec3 const*) to x3 and sets the internal helper's predictive-sound bool.
// Recognize the public three-argument GameMode::attack adapter by its ABI
// shuffle and its control-flow edge to the independently resolved four-argument
// helper. Recent builds may tail-branch (B) or make a normal call (BL) and return.
// The vtable candidate is still called through its public three-argument ABI.
bool isPublicAttackAdapter(std::uintptr_t entry, std::uintptr_t* implementation = nullptr,
                           bool* trialModeGuarded = nullptr) {
#if defined(__aarch64__)
    if (implementation) *implementation = 0;
    if (trialModeGuarded) *trialModeGuarded = false;
    entry = canonicalArm64Entry(entry);
    if (!entry || !isMinecraftFunction(reinterpret_cast<const void*>(entry))) return false;

    // These byte-pattern resolvers identify the four-argument implementation
    // (this, Actor*, predictiveSound, Vec3 const*), not the public wrapper.
    // They are used only to validate the wrapper's direct branch/call target;
    // dispatch always invokes the matching live vtable entry instead.
    static const std::array<std::uintptr_t, 2> knownImplementations = [] {
        return std::array<std::uintptr_t, 2>{
            bedrocktools::memory::resolve(SignatureId::GameModeAttackInternal),
            bedrocktools::memory::resolve(SignatureId::GameModeAttack)
        };
    }();

    auto matchesKnownImplementation = [&](std::uintptr_t target) {
        if (!target || !isMinecraftFunction(reinterpret_cast<const void*>(target))) return false;
        for (const auto expected : knownImplementations) {
            if (!expected) continue;
            if (target == expected ||
                canonicalArm64Entry(target) == canonicalArm64Entry(expected) ||
                boundedArm64BranchReaches(target, expected)) {
                return true;
            }
        }
        return false;
    };

    bool forwardsPosition = false;
    bool setsPredictiveFlag = false;
    bool trialGuard = false;
    for (std::size_t i = 0; i < 64; ++i) {
        const auto pc = entry + i * sizeof(std::uint32_t);
        if (!isReadableMemory(reinterpret_cast<const void*>(pc), sizeof(std::uint32_t))) return false;
        std::uint32_t ins = 0;
        std::memcpy(&ins, reinterpret_cast<const void*>(pc), sizeof(ins));

        if (i == 0 && ins == 0x39432008u) trialGuard = true; // ldrb w8,[x0,#0xc8]
        if (ins == 0xAA0203E3u) forwardsPosition = true;      // mov x3,x2
        // mov w2,#1; mov w2,#0; orr w2,wzr,wzr. Both predictive-sound
        // values are legitimate on different runtime builds.
        if (ins == 0x52800022u || ins == 0x52800002u ||
            ins == 0x2A1F03E2u) {
            setsPredictiveFlag = true;
        }

        const auto opcode = ins & 0xFC000000u;
        const bool directBranch = opcode == 0x14000000u; // B imm26
        const bool directCall = opcode == 0x94000000u;   // BL imm26
        if (!directBranch && !directCall) {
            // Do not scan data after a real return. Calls may occur before
            // the final RET, which is why BL is considered separately above.
            if ((ins & 0xFFFFFC1Fu) == 0xD65F0000u) break;
            continue;
        }

        // A public adapter must perform the complete argument shuffle before
        // transferring control. Unrelated calls are ignored; an unconditional
        // branch before the shuffle ends this candidate's control-flow path.
        if (!forwardsPosition || !setsPredictiveFlag) {
            if (directBranch) break;
            continue;
        }

        std::int64_t imm = static_cast<std::int64_t>(ins & 0x03FFFFFFu);
        if (imm & (1LL << 25)) imm -= (1LL << 26);
        const auto target = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(pc) + imm * 4);
        if (!target || !matchesKnownImplementation(target)) {
            if (directBranch) break;
            continue;
        }

        if (implementation) *implementation = target;
        if (trialModeGuarded) *trialModeGuarded = trialGuard;
        return true;
    }
#else
    (void)entry; (void)implementation; (void)trialModeGuarded;
#endif
    return false;
}
// Forward declaration; the exact useItem slot acts as a vtable-layout anchor for the public ABI fallback.
std::uintptr_t publicUseItemVirtualEntry(void* gameMode, std::size_t* matchedSlot);

// Find the exact public wrapper in the object's live vtable. Prefer exported signatures and
// recognized adapters; when these symbols are stripped, use the current public GameMode
// declaration's vtable order, anchored by the independently validated useItem method.
std::uintptr_t publicAttackVirtualEntry(void* gameMode, AttackKind* kind = nullptr,
                                        std::size_t* matchedSlot = nullptr,
                                        std::uintptr_t* implementationOut = nullptr) {
    if (implementationOut) *implementationOut = 0;
    if (!plausibleGameMode(gameMode)) return 0;
    void** vtable = nullptr;
    std::memcpy(&vtable, gameMode, sizeof(vtable));
    struct Symbols { std::uintptr_t base{}, survival{}; };
    static const Symbols symbols = [] {
        Symbols out{};
        constexpr const char* baseNames[] = {
            "_ZN8GameMode6attackEP5ActorRK4Vec3", "_ZN8GameMode6attackER5ActorRK4Vec3",
            "_ZN8GameMode7$attackEP5ActorRK4Vec3", "_ZN8GameMode7$attackER5ActorRK4Vec3"
        };
        constexpr const char* survivalNames[] = {
            "_ZN12SurvivalMode6attackEP5ActorRK4Vec3", "_ZN12SurvivalMode6attackER5ActorRK4Vec3",
            "_ZN12SurvivalMode7$attackEP5ActorRK4Vec3", "_ZN12SurvivalMode7$attackER5ActorRK4Vec3"
        };
        for (auto name : baseNames) if (auto p = resolveMinecraftFunction(name)) { out.base = reinterpret_cast<std::uintptr_t>(p); break; }
        for (auto name : survivalNames) if (auto p = resolveMinecraftFunction(name)) { out.survival = reinterpret_cast<std::uintptr_t>(p); break; }
        return out;
    }();
    static std::atomic<void*> cachedObject{nullptr}, cachedVtable{nullptr};
    static std::atomic<std::uintptr_t> cachedEntry{0}, cachedImplementation{0};
    static std::atomic<std::size_t> cachedSlot{0};
    static std::atomic<int> cachedKind{-1};
    static std::atomic<std::int64_t> cachedMissAtNs{0};
    if (cachedObject.load(std::memory_order_acquire) == gameMode &&
        cachedVtable.load(std::memory_order_acquire) == vtable) {
        const int k = cachedKind.load(std::memory_order_acquire);
        if (k > 0) {
            const auto e = cachedEntry.load(std::memory_order_relaxed);
            if (!e) return 0;
            if (kind) *kind = k == 2 ? AttackKind::SurvivalMode : AttackKind::GameMode;
            if (matchedSlot) *matchedSlot = cachedSlot.load(std::memory_order_relaxed);
            if (implementationOut) *implementationOut = cachedImplementation.load(std::memory_order_relaxed);
            return e;
        }
        // Do not permanently cache a route miss: a candidate may first be seen
        // while mappings/exports are still settling. Retry at a conservative
        // five-second cadence; AutoHit also backs off so this validation is not
        // repeated on a hot tick path when this runtime has no recognized route.
        const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto missNs = cachedMissAtNs.load(std::memory_order_acquire);
        if (k == 0 && nowNs >= missNs && nowNs - missNs < 5000000000LL) return 0;
    }
    // Avoid a 64-slot / 64-instruction scan every retry when this runtime strips
    // both public method exports. The ABI-anchored slot fallback below handles that case.
    if (symbols.base || symbols.survival) {
        for (std::size_t i = 0; i < 64; ++i) {
            const auto* slot = reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(vtable) + i * sizeof(void*));
            if (!isReadableMemory(slot, sizeof(void*))) break;
            std::uintptr_t address = 0;
            std::memcpy(&address, slot, sizeof(address));
            if (!address) break;
            const auto canon = canonicalArm64Entry(address);
            const bool survivalSymbol = symbols.survival && (address == symbols.survival ||
                canon == canonicalArm64Entry(symbols.survival) || boundedArm64BranchReaches(address, symbols.survival));
            const bool baseSymbol = symbols.base && (address == symbols.base ||
                canon == canonicalArm64Entry(symbols.base) || boundedArm64BranchReaches(address, symbols.base));
            std::uintptr_t internal = 0; bool trialGuard = false;
            const bool adapter = isPublicAttackAdapter(address, &internal, &trialGuard);
            if (!survivalSymbol && !baseSymbol && !adapter) continue;
            const AttackKind found = (survivalSymbol || (adapter && trialGuard))
                ? AttackKind::SurvivalMode : AttackKind::GameMode;
            cachedEntry.store(address, std::memory_order_relaxed);
            cachedImplementation.store(adapter ? internal : 0, std::memory_order_relaxed);
            cachedSlot.store(i, std::memory_order_relaxed);
            cachedKind.store(found == AttackKind::SurvivalMode ? 2 : 1, std::memory_order_relaxed);
            cachedVtable.store(vtable, std::memory_order_relaxed);
            cachedObject.store(gameMode, std::memory_order_release);
            if (kind) *kind = found;
            if (matchedSlot) *matchedSlot = i;
            if (implementationOut) *implementationOut = adapter ? internal : 0;
            static std::atomic_bool logged{false};
            if (!logged.exchange(true, std::memory_order_acq_rel)) {
                char msg[200]{};
                std::snprintf(msg, sizeof(msg), "validated public 3-argument attack route: slot=%zu; adapter=%s; internal_target=%p",
                    i, adapter ? (trialGuard ? "survival" : "base") : "symbol", reinterpret_cast<void*>(internal));
                gameHooksLogger().info("{}", msg);
            }
            return address;
        }
    
    }

    // LeviLamina's public GameMode declaration orders the virtuals as:
    // destructor(2 ABI slots), startDestroy/destroy/continue/stop, build/start,
    // tick, getPickRange, useItem, useItemAsAttack, useItemOn, interact, attack.
    // On this ABI that places useItem at vtable slot 12 and attack at slot 16.
    // We do not trust the numeric slot by itself: slot 12 must first match the
    // independently resolved public useItem method, and slots 13..16 must all be
    // readable native functions in libminecraftpe.so.
    std::size_t useItemSlot = 0;
    const auto useItemEntry = publicUseItemVirtualEntry(gameMode, &useItemSlot);
    if (useItemEntry && useItemSlot == 12) {
        std::array<std::uintptr_t, 4> adjacent{};
        bool adjacentValid = true;
        for (std::size_t slotIndex = 13; slotIndex <= 16; ++slotIndex) {
            const auto* slot = reinterpret_cast<const void*>(
                reinterpret_cast<std::uintptr_t>(vtable) + slotIndex * sizeof(void*));
            std::uintptr_t entry = 0;
            if (!isReadableMemory(slot, sizeof(void*))) {
                adjacentValid = false;
                break;
            }
            std::memcpy(&entry, slot, sizeof(entry));
            if (!entry || !isMinecraftFunction(reinterpret_cast<const void*>(entry)) ||
                !isReadableMemory(reinterpret_cast<const void*>(entry), sizeof(std::uint32_t))) {
                adjacentValid = false;
                break;
            }
            adjacent[slotIndex - 13] = entry;
        }
        if (adjacentValid) {
            const auto publicAttackEntry = adjacent[3];
            std::uintptr_t internal = 0;
            bool trialGuard = false;
            const bool adapter = isPublicAttackAdapter(publicAttackEntry, &internal, &trialGuard);
            // SurvivalMode's guarded implementation can be identified even if
            // its public wrapper is not the simple adapter shape above.
            if (!trialGuard) {
                const auto canonical = canonicalArm64Entry(publicAttackEntry);
                std::uint32_t firstInstruction = 0;
                if (canonical && isReadableMemory(reinterpret_cast<const void*>(canonical), sizeof(firstInstruction))) {
                    std::memcpy(&firstInstruction, reinterpret_cast<const void*>(canonical), sizeof(firstInstruction));
                    trialGuard = firstInstruction == 0x39432008u; // ldrb w8,[x0,#0xc8]
                }
            }
            const AttackKind found = trialGuard ? AttackKind::SurvivalMode : AttackKind::GameMode;
            cachedEntry.store(publicAttackEntry, std::memory_order_relaxed);
            cachedImplementation.store(adapter ? internal : 0, std::memory_order_relaxed);
            cachedSlot.store(16, std::memory_order_relaxed);
            cachedKind.store(found == AttackKind::SurvivalMode ? 2 : 1, std::memory_order_relaxed);
            cachedVtable.store(vtable, std::memory_order_relaxed);
            cachedObject.store(gameMode, std::memory_order_release);
            if (kind) *kind = found;
            if (matchedSlot) *matchedSlot = 16;
            if (implementationOut) *implementationOut = adapter ? internal : 0;
            gameHooksLogger().info(
                "Validated public GameMode::attack through ABI vtable layout: useItem_slot=12, attack_slot=16, adapter={}",
                adapter ? "recognized" : "anchored_by_useItem_and_neighbor_slots");
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
                "GameHooks", "attack_route", "public_attack_validated_by_vtable_layout",
                std::string("useItem_slot=12; attack_slot=16; adapter=") + (adapter ? "recognized" : "layout_anchor"));
            return publicAttackEntry;
        }
    }

    cachedEntry.store(0, std::memory_order_relaxed);
    cachedImplementation.store(0, std::memory_order_relaxed);
    cachedSlot.store(0, std::memory_order_relaxed);
    cachedKind.store(0, std::memory_order_relaxed);
    cachedVtable.store(vtable, std::memory_order_relaxed);
    cachedObject.store(gameMode, std::memory_order_release);
    cachedMissAtNs.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_release);
    static std::atomic_bool miss{false};
    if (!miss.exchange(true, std::memory_order_acq_rel)) {
        std::uintptr_t s0=0,s2=0,s16=0,s32=0;
        auto readSlot=[&](std::size_t n,std::uintptr_t& out){auto a=reinterpret_cast<std::uintptr_t>(vtable)+n*sizeof(void*); if(isReadableMemory(reinterpret_cast<const void*>(a),sizeof(void*)))std::memcpy(&out,reinterpret_cast<const void*>(a),sizeof(out));};
        readSlot(0,s0); readSlot(2,s2); readSlot(16,s16); readSlot(32,s32);
        const auto internalSignature = bedrocktools::memory::resolve(SignatureId::GameModeAttackInternal);
        const auto legacyAttackSignature = bedrocktools::memory::resolve(SignatureId::GameModeAttack);
        gameHooksLogger().warn("No validated public 3-argument attack route: object={:p}, vtable={:p}, public_sig={:p}, survival_sig={:p}, internal_sig={:p}, legacy_attack_sig={:p}, slot0={:p}, slot2={:p}, slot16={:p}, slot32={:p}.",
            gameMode, reinterpret_cast<const void*>(vtable), reinterpret_cast<void*>(symbols.base), reinterpret_cast<void*>(symbols.survival),
            reinterpret_cast<void*>(internalSignature), reinterpret_cast<void*>(legacyAttackSignature),
            reinterpret_cast<void*>(s0), reinterpret_cast<void*>(s2), reinterpret_cast<void*>(s16), reinterpret_cast<void*>(s32));
    }
    return 0;
}

std::uintptr_t publicUseItemVirtualEntry(void* gameMode, std::size_t* matchedSlot) {
    if (!plausibleGameMode(gameMode)) return 0;
    void** vtable = nullptr;
    std::memcpy(&vtable, gameMode, sizeof(vtable));
    const auto baseSignature = bedrocktools::memory::resolve(SignatureId::GameModeUseItem);
    const auto survivalSignature = bedrocktools::memory::resolve(SignatureId::SurvivalModeUseItem);
    struct Exported { std::uintptr_t base{}; std::uintptr_t survival{}; };
    static const Exported symbols = [] {
        Exported out{};
        constexpr const char* baseNames[] = {
            "_ZN8GameMode7useItemEP9ItemStack", "_ZN8GameMode7useItemER9ItemStack",
            "_ZN8GameMode8$useItemEP9ItemStack", "_ZN8GameMode8$useItemER9ItemStack"
        };
        constexpr const char* survivalNames[] = {
            "_ZN12SurvivalMode7useItemEP9ItemStack", "_ZN12SurvivalMode7useItemER9ItemStack",
            "_ZN12SurvivalMode8$useItemEP9ItemStack", "_ZN12SurvivalMode8$useItemER9ItemStack"
        };
        for (const char* name : baseNames) {
            if (void* address = resolveMinecraftFunction(name)) {
                out.base = reinterpret_cast<std::uintptr_t>(address);
                break;
            }
        }
        for (const char* name : survivalNames) {
            if (void* address = resolveMinecraftFunction(name)) {
                out.survival = reinterpret_cast<std::uintptr_t>(address);
                break;
            }
        }
        return out;
    }();

    // This resolver is reached repeatedly while AutoHit is enabled. Cache both
    // exact matches and misses per object/vtable so a missing optional method
    // cannot trigger a full scan of 64 slots on every local-player tick.
    static std::atomic<void*> cachedObject{nullptr}, cachedVtable{nullptr};
    static std::atomic<std::uintptr_t> cachedEntry{0};
    static std::atomic<std::size_t> cachedSlot{0};
    static std::atomic<int> cachedState{-1}; // -1 unknown, 0 miss, 1 hit
    static std::atomic<std::int64_t> cachedMissAtNs{0};
    const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (cachedObject.load(std::memory_order_acquire) == gameMode &&
        cachedVtable.load(std::memory_order_acquire) == vtable) {
        const int state = cachedState.load(std::memory_order_acquire);
        if (state == 1) {
            const auto slotIndex = cachedSlot.load(std::memory_order_relaxed);
            const auto entry = cachedEntry.load(std::memory_order_relaxed);
            const auto* slot = reinterpret_cast<const void*>(
                reinterpret_cast<std::uintptr_t>(vtable) + slotIndex * sizeof(void*));
            std::uintptr_t liveEntry = 0;
            if (entry && isReadableMemory(slot, sizeof(void*))) {
                std::memcpy(&liveEntry, slot, sizeof(liveEntry));
                if (liveEntry == entry) {
                    if (matchedSlot) *matchedSlot = slotIndex;
                    return entry;
                }
            }
        } else if (state == 0) {
            const auto missNs = cachedMissAtNs.load(std::memory_order_acquire);
            if (nowNs >= missNs && nowNs - missNs < 5000000000LL) return 0;
        }
    }

    for (std::size_t i = 0; i < 64; ++i) {
        const auto* slot = reinterpret_cast<const void*>(
            reinterpret_cast<std::uintptr_t>(vtable) + i * sizeof(void*));
        if (!isReadableMemory(slot, sizeof(void*))) break;
        std::uintptr_t address = 0;
        std::memcpy(&address, slot, sizeof(address));
        if (!address) break;
        const auto matchesPublicUseItem = [&](std::uintptr_t candidate, std::uintptr_t target) {
            if (!target) return false;
            return candidate == target ||
                canonicalArm64Entry(candidate) == canonicalArm64Entry(target) ||
                boundedArm64BranchReaches(candidate, target);
        };
        if (matchesPublicUseItem(address, baseSignature) ||
            matchesPublicUseItem(address, symbols.base) ||
            matchesPublicUseItem(address, survivalSignature) ||
            matchesPublicUseItem(address, symbols.survival)) {
            cachedEntry.store(address, std::memory_order_relaxed);
            cachedSlot.store(i, std::memory_order_relaxed);
            cachedState.store(1, std::memory_order_relaxed);
            cachedVtable.store(vtable, std::memory_order_relaxed);
            cachedObject.store(gameMode, std::memory_order_release);
            if (matchedSlot) *matchedSlot = i;
            static std::atomic_bool loggedMatch{false};
            if (!loggedMatch.exchange(true, std::memory_order_acq_rel))
                gameHooksLogger().info("Validated public useItem through exact vtable match at slot {}.", i);
            return address;
        }
    }

    cachedEntry.store(0, std::memory_order_relaxed);
    cachedSlot.store(0, std::memory_order_relaxed);
    cachedMissAtNs.store(nowNs, std::memory_order_relaxed);
    cachedState.store(0, std::memory_order_relaxed);
    cachedVtable.store(vtable, std::memory_order_relaxed);
    cachedObject.store(gameMode, std::memory_order_release);
    static std::atomic_bool loggedMiss{false};
    if (!loggedMiss.exchange(true, std::memory_order_acq_rel))
        gameHooksLogger().warn("No exact public GameMode::useItem method found in the live vtable.");
    return 0;
}

AttackKind attackKindFor(void* gameMode) {
    AttackKind kind = AttackKind::GameMode;
    publicAttackVirtualEntry(gameMode, &kind);
    return kind;
}

std::string versionDetour(void* self) {
    std::string version = versionOriginal ? versionOriginal(self) : std::string{};
    if (gameVersion.empty()) gameVersion = version;
    return std::string("\xC2\xA7" "b") + std::string(bedrocktools::Name) + " v" + std::string(bedrocktools::Version) + " " + "\xC2\xA7" "fby " + "\xC2\xA7" "e" + std::string(bedrocktools::Author) + " " + "\xC2\xA7" "f- " + "\xC2\xA7" "r" + version;
}

bool isLocalPlayerTickObject(void* actor) {
    if (!actor || !isReadableMemory(actor, sizeof(void*))) return false;

    void** vtable = nullptr;
    std::memcpy(&vtable, actor, sizeof(vtable));
    if (!vtable || !isReadableMemory(vtable, sizeof(void*) * 64)) return false;

    static const auto normalTick = bedrocktools::memory::resolve(SignatureId::NormalTick);
    if (normalTick == 0 || !isMinecraftFunction(reinterpret_cast<const void*>(normalTick)))
        return false;

    // Prefer exact identity: it is cheap and avoids canonicalizing every vtable
    // entry on the hot path. Some ARM64 builds place a short B-branch veneer in
    // the vtable, however, so exact equality alone can reject the actual
    // LocalPlayer vtable even though the veneer resolves to the scanned method.
    for (std::size_t i = 0; i < 64; ++i) {
        std::uintptr_t entry = 0;
        std::memcpy(&entry, vtable + i, sizeof(entry));
        if (entry == normalTick) return true;
    }

    const auto canonicalNormalTick = canonicalArm64Entry(normalTick);
    if (!canonicalNormalTick) return false;
    for (std::size_t i = 0; i < 64; ++i) {
        std::uintptr_t entry = 0;
        std::memcpy(&entry, vtable + i, sizeof(entry));
        if (!entry || entry == normalTick) continue;
        if (canonicalArm64Entry(entry) == canonicalNormalTick) {
            static std::atomic_bool loggedThunkMatch{false};
            if (!loggedThunkMatch.exchange(true, std::memory_order_acq_rel)) {
                bedrocktools::diagnostics::probe(
                    bedrocktools::diagnostics::Level::Info, "GameHooks",
                    "verified_local_player", "captured_from_normal_tick_thunk",
                    std::string("vtable_slot=") + std::to_string(i));
            }
            return true;
        }
    }

    static std::atomic_bool loggedVtableMismatch{false};
    if (!loggedVtableMismatch.exchange(true, std::memory_order_acq_rel)) {
        // Diagnostic-only: sample the first 64 entries so version drift can be
        // investigated without accepting an unverified LocalPlayer vtable.
        std::string detail = std::string("actor=") +
            std::to_string(reinterpret_cast<std::uintptr_t>(actor)) +
            ";vtable=" + std::to_string(reinterpret_cast<std::uintptr_t>(vtable)) +
            ";normal_tick=" + std::to_string(normalTick) +
            ";canonical_tick=" + std::to_string(canonicalNormalTick) + ";entries=";
        std::size_t reported = 0;
        for (std::size_t i = 0; i < 64; ++i) {
            std::uintptr_t entry = 0;
            std::memcpy(&entry, vtable + i, sizeof(entry));
            if (!entry) continue;
            const auto canonical = canonicalArm64Entry(entry);
            if (!isMinecraftFunction(reinterpret_cast<const void*>(entry)) &&
                canonical == entry) continue;
            if (reported++) detail += ",";
            detail += std::to_string(i) + ":" + std::to_string(entry) +
                "->" + std::to_string(canonical);
            if (detail.size() > 1800) {
                detail += ",truncated";
                break;
            }
        }
        if (reported == 0) detail += "none-classified-as-minecraft";
        bedrocktools::diagnostics::probe(
            bedrocktools::diagnostics::Level::Warning, "GameHooks",
            "verified_local_player", "normal_tick_vtable_mismatch", detail);
    }
    return false;
}

bool clientInstanceDirectlyOwnsPlayer(void* clientInstance, void* actor, std::size_t* matchedOffset) {
    if (!clientInstance || !actor ||
        !isReadableMemory(clientInstance, sizeof(void*)) ||
        !isReadableMemory(actor, sizeof(void*))) return false;

    // Do not infer LocalPlayer from a generic Actor::normalTick vtable match alone.
    // The current ClientInstance is captured from its real update detour; validate
    // its vtable and require the candidate Actor pointer to be stored directly in
    // the live ClientInstance object.
    void** clientVtable = nullptr;
    std::memcpy(&clientVtable, clientInstance, sizeof(clientVtable));
    if (!clientVtable || !isReadableMemory(clientVtable, sizeof(void*) * 32)) return false;

    bool trustedClientVtable = false;
    for (std::size_t i = 0; i < 32; ++i) {
        std::uintptr_t entry = 0;
        std::memcpy(&entry, clientVtable + i, sizeof(entry));
        if (entry && isMinecraftFunction(reinterpret_cast<const void*>(entry))) {
            trustedClientVtable = true;
            break;
        }
    }
    if (!trustedClientVtable) return false;

    // Only actual Player actors are candidates. This resolved classifier is used
    // elsewhere by BedrockAim and prevents arbitrary entity pointers from being
    // promoted to the local-player slot by a coincidental member match.
    static const auto actorIsPlayerAddress = bedrocktools::memory::resolve(SignatureId::ActorIsPlayer);
    if (!actorIsPlayerAddress ||
        !isMinecraftFunction(reinterpret_cast<const void*>(actorIsPlayerAddress))) return false;
    using ActorIsPlayerFn = bool (*)(void*);
    const auto actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(actorIsPlayerAddress);
    if (!actorIsPlayer(actor)) return false;

    // ClientInstance layouts move between Bedrock builds and platform ABIs.
    // Search a bounded prefix for the exact actor pointer instead of calling the
    // known-stale legacy ClientInstanceGetLocalPlayer signature or guessing a
    // version-sensitive field offset. The field is compared as data; no guessed
    // candidate pointer is dereferenced or called.
    const auto scanObjectForExactPlayerPointer = [&](void* object, std::size_t scanLimit,
                                                        std::size_t* foundOffset) {
        if (!object || !isReadableMemory(object, sizeof(void*))) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(object);
        for (std::size_t offset = sizeof(void*); offset < scanLimit; offset += alignof(void*)) {
            if (base > std::numeric_limits<std::uintptr_t>::max() - offset) break;
            const auto* field = reinterpret_cast<const void*>(base + offset);
            if (!isReadableMemory(field, sizeof(void*))) continue;
            void* value = nullptr;
            std::memcpy(&value, field, sizeof(value));
            if (value != actor) continue;
            if (foundOffset) *foundOffset = offset;
            return true;
        }
        return false;
    };

    // Direct fields are the cheapest path and cover layouts with an inline
    // LocalPlayer pointer.
    if (scanObjectForExactPlayerPointer(clientInstance, 0x400, matchedOffset)) return true;

    // Some Bedrock layouts expose the local player through MinecraftGame rather
    // than storing the pointer directly on ClientInstance. This is an existing,
    // version-profiled virtual accessor (slot 83 in Core.hpp), not a guessed
    // LocalPlayer getter. Validate the returned object before comparing its fields.
    auto* sdkClient = reinterpret_cast<bedrocktools::sdk::ClientInstance*>(clientInstance);
    void* minecraftGame = sdkClient->minecraftGame();
    if (minecraftGame && minecraftGame != clientInstance &&
        isReadableMemory(minecraftGame, sizeof(void*))) {
        void** gameVtable = nullptr;
        std::memcpy(&gameVtable, minecraftGame, sizeof(gameVtable));
        bool trustedGameVtable = false;
        if (gameVtable && isReadableMemory(gameVtable, sizeof(void*) * 16)) {
            for (std::size_t i = 0; i < 16; ++i) {
                std::uintptr_t entry = 0;
                std::memcpy(&entry, gameVtable + i, sizeof(entry));
                if (entry && isMinecraftFunction(reinterpret_cast<const void*>(entry))) {
                    trustedGameVtable = true;
                    break;
                }
            }
        }
        if (trustedGameVtable) {
            std::size_t nestedOffset = 0;
            if (scanObjectForExactPlayerPointer(minecraftGame, 0x800, &nestedOffset)) {
                if (matchedOffset) *matchedOffset = 0x10000 + nestedOffset;
                return true;
            }
        }
    }
    return false;
}

void* resolveLocalPlayerFromClientInstance(void* clientInstance) {
    if (!clientInstance || !isReadableMemory(clientInstance, sizeof(void*))) {
        bedrocktools::diagnostics::probe(
            bedrocktools::diagnostics::Level::Warning, "GameHooks",
            "verified_local_player", "client_instance_missing",
            "cannot resolve LocalPlayer because the captured ClientInstance pointer is null or unreadable");
        return nullptr;
    }

    using GetLocalPlayerFn = void* (*)(void*);
    using ActorIsPlayerFn = bool (*)(void*);

    // Prefer exact exported C++ symbols if this game build exposes them. Do not
    // call the older byte-pattern signature here: it was observed to match an
    // unrelated JNI function on a nearby Bedrock build.
    static GetLocalPlayerFn exportedGetter = []() -> GetLocalPlayerFn {
        constexpr const char* names[] = {
            "_ZNK14ClientInstance14getLocalPlayerEv",
            "_ZN14ClientInstance14getLocalPlayerEv",
            "_ZNK14ClientInstance15getLocalPlayerEv",
            "_ZN14ClientInstance15getLocalPlayerEv"
        };
        for (const char* name : names) {
            if (void* address = resolveMinecraftFunction(name)) {
                bedrocktools::diagnostics::probe(
                    bedrocktools::diagnostics::Level::Info, "GameHooks",
                    "verified_local_player", "exact_getter_symbol_found", name);
                return reinterpret_cast<GetLocalPlayerFn>(address);
            }
        }
        bedrocktools::diagnostics::probe(
            bedrocktools::diagnostics::Level::Debug, "GameHooks",
            "verified_local_player", "exact_getter_symbol_missing",
            "no exported ClientInstance::getLocalPlayer symbol; using validated vtable fallback");
        return nullptr;
    }();

    static const auto actorIsPlayerAddress =
        bedrocktools::memory::resolve(SignatureId::ActorIsPlayer);
    const bool actorClassifierReady = actorIsPlayerAddress &&
        isMinecraftFunction(reinterpret_cast<const void*>(actorIsPlayerAddress));
    auto acceptCandidate = [&](void* candidate, const char* route) -> bool {
        if (!candidate || !isReadableMemory(candidate, sizeof(void*))) return false;
        if (!actorClassifierReady) {
            bedrocktools::diagnostics::probe(
                bedrocktools::diagnostics::Level::Warning, "GameHooks",
                "verified_local_player", "actor_classifier_unavailable",
                "getter returned a readable candidate, but Actor::isPlayer could not be validated");
            return false;
        }
        const bool isPlayer = reinterpret_cast<ActorIsPlayerFn>(actorIsPlayerAddress)(candidate);
        if (!isPlayer) {
            bedrocktools::diagnostics::probe(
                bedrocktools::diagnostics::Level::Warning, "GameHooks",
                "verified_local_player", "getter_candidate_rejected",
                std::string("route=") + route + "; Actor::isPlayer returned false");
            return false;
        }

        currentLocalPlayer.store(candidate, std::memory_order_release);
        bedrocktools::diagnostics::probe(
            bedrocktools::diagnostics::Level::Info, "GameHooks",
            "verified_local_player",
            std::string("captured_from_") + route,
            std::string("client_instance=") +
                std::to_string(reinterpret_cast<std::uintptr_t>(clientInstance)) +
                "; player=" + std::to_string(reinterpret_cast<std::uintptr_t>(candidate)));
        return true;
    };

    // An exported symbol is a stronger target than a guessed virtual index.
    if (exportedGetter) {
        void* candidate = exportedGetter(clientInstance);
        if (acceptCandidate(candidate, "clientinstance_exported_getter")) return candidate;
        if (!candidate) {
            bedrocktools::diagnostics::probe(
                bedrocktools::diagnostics::Level::Debug, "GameHooks",
                "verified_local_player", "exported_getter_returned_null",
                "exact exported ClientInstance::getLocalPlayer returned null for the captured instance");
        }
    }

    // The supported ClientInstance interface places getLocalPlayer immediately
    // after getRegion. Validate the object/vtable storage before reading slot 32.
    void** vtable = nullptr;
    std::memcpy(&vtable, clientInstance, sizeof(vtable));
    constexpr std::size_t localPlayerSlot =
        bedrocktools::sdk::offsets::VTable::ClientInstance_getLocalPlayer;
    void* nativeGetter = nullptr;
    if (vtable && isReadableMemory(vtable, sizeof(void*) * (localPlayerSlot + 1))) {
        std::memcpy(&nativeGetter, vtable + localPlayerSlot, sizeof(nativeGetter));
    }

    bool vtableGetterCallable = nativeGetter && isMinecraftFunction(nativeGetter);
    if (!vtableGetterCallable && nativeGetter) {
        const auto canonical = canonicalArm64Entry(reinterpret_cast<std::uintptr_t>(nativeGetter));
        vtableGetterCallable = canonical && canonical != reinterpret_cast<std::uintptr_t>(nativeGetter) &&
            isMinecraftFunction(reinterpret_cast<const void*>(canonical));
        if (vtableGetterCallable) nativeGetter = reinterpret_cast<void*>(canonical);
    }

    if (vtableGetterCallable) {
        void* candidate = reinterpret_cast<GetLocalPlayerFn>(nativeGetter)(clientInstance);
        if (acceptCandidate(candidate, "clientinstance_vtable_getter")) return candidate;
        if (!candidate) {
            bedrocktools::diagnostics::probe(
                bedrocktools::diagnostics::Level::Debug, "GameHooks",
                "verified_local_player", "vtable_getter_returned_null",
                std::string("slot=") + std::to_string(localPlayerSlot) +
                    "; client_instance=" +
                    std::to_string(reinterpret_cast<std::uintptr_t>(clientInstance)));
        }
    } else {
        bedrocktools::diagnostics::probe(
            bedrocktools::diagnostics::Level::Warning, "GameHooks",
            "verified_local_player", "clientinstance_vtable_getter_unavailable",
            std::string("slot=") + std::to_string(localPlayerSlot) +
                "; getter=" + std::to_string(reinterpret_cast<std::uintptr_t>(nativeGetter)));
    }

    // Keep only a player captured by one of the independently validated routes.
    void* cachedPlayer = currentLocalPlayer.load(std::memory_order_acquire);
    if (cachedPlayer && isReadableMemory(cachedPlayer, sizeof(void*))) return cachedPlayer;

    bedrocktools::diagnostics::probe(
        bedrocktools::diagnostics::Level::Warning, "GameHooks",
        "verified_local_player", "resolution_failed",
        std::string("client_instance=") +
            std::to_string(reinterpret_cast<std::uintptr_t>(clientInstance)) +
            "; exact_export=" + (exportedGetter ? "found" : "missing") +
            "; vtable_getter=" + (vtableGetterCallable ? "callable" : "unavailable") +
            "; actor_classifier=" + (actorClassifierReady ? "ready" : "unavailable"));
    return nullptr;
}

void tickDetour(void* actor) {
    // Per-actor hook: cached identity comparison only; never scan ClientInstance here.
    auto* actorPlayer = reinterpret_cast<bedrocktools::sdk::Player*>(actor);
    void* local = currentLocalPlayer.load(std::memory_order_acquire);
    const bool verified = actor && local && actor == local;
    if (verified) {
        LocalPlayerPreTickEvent pre{actorPlayer};
        bus().publish(pre);
        VerifiedLocalPlayerPreTickEvent verifiedPre{reinterpret_cast<bedrocktools::sdk::Player*>(local)};
        bus().publish(verifiedPre);
    }

    if (tickOriginal) tickOriginal(actor);

    if (verified) {
        bedaim::events::VerifiedLocalPlayerTickEvent postTick{
            reinterpret_cast<bedrocktools::sdk::Player*>(local)};
        bus().publish(postTick);
        LocalPlayerTickEvent localTick{actorPlayer};
        bus().publish(localTick);
    }
}

bool gameModeStartDestroyBlockDetour(void* gameMode, const void* position, std::uint8_t face, bool* destroyed) {
    GameModeActionScope scope{GameModeType::GameMode, gameMode};
    const bool result = gameModeStartDestroyBlockOriginal ? gameModeStartDestroyBlockOriginal(gameMode, position, face, destroyed) : false;
    scope.publish(GameModeAction::StartDestroyBlock);
    return result;
}

bool survivalModeStartDestroyBlockDetour(void* gameMode, const void* position, std::uint8_t face, bool* destroyed) {
    GameModeActionScope scope{GameModeType::SurvivalMode, gameMode};
    const bool result = survivalModeStartDestroyBlockOriginal ? survivalModeStartDestroyBlockOriginal(gameMode, position, face, destroyed) : false;
    scope.publish(GameModeAction::StartDestroyBlock);
    return result;
}

void gameModeStopDestroyBlockDetour(void* gameMode, const void* position) {
    if (gameMode) currentGameMode.store(gameMode, std::memory_order_release);
    const auto modeKind = attackKindFor(gameMode) == AttackKind::SurvivalMode ? GameModeType::SurvivalMode : GameModeType::GameMode;
    if (gameModeStopDestroyBlockOriginal) gameModeStopDestroyBlockOriginal(gameMode, position);
    GameModeActionEvent event{modeKind, GameModeAction::StopDestroyBlock, gameMode};
    bus().publish(event);
}

void gameModeStartBuildBlockDetour(void* gameMode, const void* position, std::uint8_t face) {
    GameModeActionScope scope{GameModeType::GameMode, gameMode};
    if (gameModeStartBuildBlockOriginal) gameModeStartBuildBlockOriginal(gameMode, position, face);
    scope.publish(GameModeAction::StartBuildBlock);
}

void survivalModeStartBuildBlockDetour(void* gameMode, const void* position, std::uint8_t face) {
    GameModeActionScope scope{GameModeType::SurvivalMode, gameMode};
    if (survivalModeStartBuildBlockOriginal) survivalModeStartBuildBlockOriginal(gameMode, position, face);
    scope.publish(GameModeAction::StartBuildBlock);
}

bool gameModeUseItemDetour(void* gameMode, void* item) {
    GameModeActionScope scope{GameModeType::GameMode, gameMode};
    const bool result = gameModeUseItemOriginal ? gameModeUseItemOriginal(gameMode, item) : false;
    scope.publish(GameModeAction::UseItem, true, result);
    return result;
}

bool survivalModeUseItemDetour(void* gameMode, void* item) {
    GameModeActionScope scope{GameModeType::SurvivalMode, gameMode};
    const bool result = survivalModeUseItemOriginal ? survivalModeUseItemOriginal(gameMode, item) : false;
    scope.publish(GameModeAction::UseItem, true, result);
    return result;
}

bool gameModeUseItemAsAttackDetour(void* gameMode, void* item, const void* direction) {
    GameModeActionScope scope{GameModeType::GameMode, gameMode};
    const bool result = gameModeUseItemAsAttackOriginal ? gameModeUseItemAsAttackOriginal(gameMode, item, direction) : false;
    if (result) scope.publish(GameModeAction::UseItemAsAttack);
    return result;
}

bool survivalModeUseItemAsAttackDetour(void* gameMode, void* item, const void* direction) {
    GameModeActionScope scope{GameModeType::SurvivalMode, gameMode};
    const bool result = survivalModeUseItemAsAttackOriginal ? survivalModeUseItemAsAttackOriginal(gameMode, item, direction) : false;
    if (result) scope.publish(GameModeAction::UseItemAsAttack);
    return result;
}

InteractionResultValue gameModeUseItemOnDetour(void* gameMode, void* item, const void* position, std::uint8_t face, const void* hit, const void* block, bool firstEvent) {
    GameModeActionScope scope{GameModeType::GameMode, gameMode};
    const auto result = gameModeUseItemOnOriginal ? gameModeUseItemOnOriginal(gameMode, item, position, face, hit, block, firstEvent) : InteractionResultValue{};
    if (firstEvent && (interactionSucceeded(result) || (interactionSwings(result) && itemStackHasItem(item)))) scope.publish(GameModeAction::UseItemOn);
    return result;
}

InteractionResultValue survivalModeUseItemOnDetour(void* gameMode, void* item, const void* position, std::uint8_t face, const void* hit, const void* block, bool firstEvent) {
    GameModeActionScope scope{GameModeType::SurvivalMode, gameMode};
    const auto result = survivalModeUseItemOnOriginal ? survivalModeUseItemOnOriginal(gameMode, item, position, face, hit, block, firstEvent) : InteractionResultValue{};
    if (firstEvent && (interactionSucceeded(result) || (interactionSwings(result) && itemStackHasItem(item)))) scope.publish(GameModeAction::UseItemOn);
    return result;
}

bool gameModeInteractDetour(void* gameMode, void* target, const void* location) {
    GameModeActionScope scope{GameModeType::GameMode, gameMode};
    const bool result = gameModeInteractOriginal ? gameModeInteractOriginal(gameMode, target, location) : false;
    if (result) scope.publish(GameModeAction::Interact);
    return result;
}

bool survivalModeInteractDetour(void* gameMode, void* target, const void* location) {
    GameModeActionScope scope{GameModeType::SurvivalMode, gameMode};
    const bool result = survivalModeInteractOriginal ? survivalModeInteractOriginal(gameMode, target, location) : false;
    if (result) scope.publish(GameModeAction::Interact);
    return result;
}

bool gameModeAttackDetour(void* gameMode, void* target, bool playPredictiveSound, const void* hitPosition) {
    static std::atomic_bool loggedNativeAttack{false};
    if (!loggedNativeAttack.exchange(true, std::memory_order_acq_rel))
        gameHooksLogger().info("Native attack detour received its first call (target={}, hitPosition={}, trampoline={}).",
            target != nullptr, hitPosition != nullptr, gameModeAttackOriginal != nullptr);
    const bool eventsAlreadyPublished = syntheticAttackEventsPrepublished;
    const AttackKind kind = attackKindFor(gameMode);
    if (!eventsAlreadyPublished) {
        const GameModeType modeKind = kind == AttackKind::SurvivalMode ? GameModeType::SurvivalMode : GameModeType::GameMode;
        GameModeActionScope scope{modeKind, gameMode};
        scope.publish(GameModeAction::Attack);
        AttackEvent event{kind, gameMode, reinterpret_cast<bedrocktools::sdk::Actor*>(target),
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(playPredictiveSound)), const_cast<void*>(hitPosition)};
        bus().publish(event);
        if (event.cancelled()) {
            static std::atomic_bool cancelled{false};
            if (!cancelled.exchange(true, std::memory_order_acq_rel))
                gameHooksLogger().warn("The first native attack was cancelled by an AttackEvent listener.");
            return false;
        }
    }
    const bool result = gameModeAttackOriginal ? gameModeAttackOriginal(gameMode, target, playPredictiveSound, hitPosition) : false;
    if (syntheticAttackDispatchActive) {
        syntheticAttackResultObserved = true;
        syntheticAttackNativeResult = result;
    }
    static std::atomic_bool loggedResult{false};
    if (!loggedResult.exchange(true, std::memory_order_acq_rel))
        gameHooksLogger().info("First native attack trampoline result: {}.", result);
    if (!eventsAlreadyPublished) {
        AttackResultEvent resultEvent{kind, reinterpret_cast<bedrocktools::sdk::Actor*>(target), result};
        bus().publish(resultEvent);
    }
    return result;
}

bool ensureAttackObserver(std::uintptr_t implementation) {
    if (!implementation ||
        !isMinecraftFunction(reinterpret_cast<const void*>(implementation)) ||
        !isReadableMemory(reinterpret_cast<const void*>(implementation), sizeof(std::uint32_t)))
        return false;

    std::lock_guard<std::mutex> lock(attackObserverMutex);
    if (attackObserverHandle &&
        attackObserverTarget.load(std::memory_order_acquire) == reinterpret_cast<void*>(implementation) &&
        gameModeAttackOriginal)
        return true;

    if (attackObserverHandle) {
        bedrocktools::hooks::remove(attackObserverHandle);
        attackObserverHandle = nullptr;
        gameModeAttackOriginal = nullptr;
        attackObserverTarget.store(nullptr, std::memory_order_release);
    }

    auto handle = bedrocktools::hooks::install(
        reinterpret_cast<void*>(implementation),
        reinterpret_cast<void*>(gameModeAttackDetour),
        reinterpret_cast<void**>(&gameModeAttackOriginal));
    if (!handle) {
        gameModeAttackOriginal = nullptr;
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "GameHooks", "attack_observer", "install_failed",
            std::string("validated_adapter_target=0x") +
                std::to_string(static_cast<unsigned long long>(implementation)));
        return false;
    }

    attackObserverHandle = handle;
    attackObserverTarget.store(reinterpret_cast<void*>(implementation), std::memory_order_release);
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "GameHooks", "attack_observer", "installed_on_validated_adapter_target",
        std::string("target=0x") +
            std::to_string(static_cast<unsigned long long>(implementation)));
    return true;
}

bool clientUpdateDetour(void* clientInstance, bool value) {
    std::size_t trustedVtableEntries = 0;
    const bool receiverReadable = clientInstance &&
        isReadableMemory(clientInstance, sizeof(void*));
    const bool receiverValid =
        isPlausibleClientInstance(clientInstance, &trustedVtableEntries);

    if (receiverValid) {
        void* previousClientInstance =
            currentClientInstance.exchange(clientInstance, std::memory_order_acq_rel);
        if (previousClientInstance != clientInstance)
            currentLocalPlayer.store(nullptr, std::memory_order_release);
        bedrocktools::diagnostics::probe(
            bedrocktools::diagnostics::Level::Info, "GameHooks",
            "client_instance_update_detour", "receiver_valid",
            std::string("receiver=") +
                std::to_string(reinterpret_cast<std::uintptr_t>(clientInstance)) +
                "; trusted_vtable_entries=" + std::to_string(trustedVtableEntries));
    } else {
        // Never poison the cached ClientInstance (or clear a valid LocalPlayer)
        // when a stale signature happens to intercept a different receiver.
        bedrocktools::diagnostics::probe(
            bedrocktools::diagnostics::Level::Warning, "GameHooks",
            "client_instance_update_detour", "receiver_rejected",
            std::string("receiver=") +
                std::to_string(reinterpret_cast<std::uintptr_t>(clientInstance)) +
                "; object_readable=" + (receiverReadable ? "yes" : "no") +
                "; trusted_vtable_entries=" + std::to_string(trustedVtableEntries));
    }

    // The current Bedrock/LeviLamina ABI is bool ClientInstance::update(bool).
    // Preserve that exact return type; returning a pointer here corrupts the
    // caller's bool result on ARM64 even though the hook itself appears installed.
    const bool result = clientUpdateOriginal
        ? clientUpdateOriginal(clientInstance, value) : false;

    if (receiverValid && shouldResolveLocalPlayer(clientInstance)) {
        // Resolve GameMode from a verified LocalPlayer and install the attack
        // observer at the public adapter's validated implementation target.
        // This happens independently of AutoHit being enabled, preserving real
        // attack events for Keystrokes/ReachCounter/SkinStealer and other modules.
        void* resolvedPlayer = resolveLocalPlayerFromClientInstance(clientInstance);
        if (resolvedPlayer) {
            void* resolvedGameMode = gameModeForPlayer(resolvedPlayer);
            if (resolvedGameMode) (void)attackAvailable(resolvedGameMode);
        }
    }
    void* publishedClientInstance = currentClientInstance.load(std::memory_order_acquire);
    ClientInstanceUpdateEvent event{
        reinterpret_cast<bedrocktools::sdk::ClientInstance*>(publishedClientInstance)};
    bus().publish(event);
    return result;
}

std::uint32_t containerSlotSelectedDetour(void* controller, const std::string& collectionName, int index) {
    ContainerSlotSelectedEvent event{controller, collectionName, index};
    bus().publish(event);
    const auto result = !event.cancelled() && containerSlotSelectedOriginal
        ? containerSlotSelectedOriginal(controller, collectionName, index) : 0;
    event.afterSelection = true;
    bus().publish(event);
    return result;
}

void* containerOpenDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    ScreenStateEvent event{ScreenKind::Container, ScreenPhase::Opened, a0};
    bus().publish(event);
    return containerOpenOriginal ? containerOpenOriginal(a0, a1, a2, a3, a4, a5, a6, a7) : nullptr;
}

void* containerCloseDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    ScreenStateEvent event{ScreenKind::Container, ScreenPhase::Closed, a0};
    bus().publish(event);
    return containerCloseOriginal ? containerCloseOriginal(a0, a1, a2, a3, a4, a5, a6, a7) : nullptr;
}

void* chatOpenDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    ScreenStateEvent event{ScreenKind::Chat, ScreenPhase::Opened, a0};
    bus().publish(event);
    return chatOpenOriginal ? chatOpenOriginal(a0, a1, a2, a3, a4, a5, a6, a7) : nullptr;
}

void* chatCloseDetour(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    ScreenStateEvent event{ScreenKind::Chat, ScreenPhase::Closed, a0};
    bus().publish(event);
    return chatCloseOriginal ? chatCloseOriginal(a0, a1, a2, a3, a4, a5, a6, a7) : nullptr;
}

EGLBoolean swapBuffersDetour(EGLDisplay display, EGLSurface surface) {
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
        FrameEvent event;
        bus().publish(event);
        // FrameEvent is observational only. A module must never suppress the
        // real EGL swap: returning EGL_TRUE without calling the original stalls
        // presentation and can conflict with BedrockTools' own render hooks.
    }
    return swapBuffersOriginal ? swapBuffersOriginal(display, surface) : EGL_FALSE;
}

bool hookEgl() {
    auto egl = bedrocktools::hooks::openLibrary("libEGL.so");
    if (!egl) return false;
    const auto address = bedrocktools::hooks::symbol(egl, "eglSwapBuffers");
    if (!address) {
        bedrocktools::hooks::closeLibrary(egl);
        return false;
    }
    auto handle = bedrocktools::hooks::install(reinterpret_cast<void*>(address), reinterpret_cast<void*>(swapBuffersDetour), reinterpret_cast<void**>(&swapBuffersOriginal));
    bedrocktools::hooks::closeLibrary(egl);
    if (!handle) return false;
    if (handleCount < handles.size()) handles[handleCount++] = handle;
    return true;
}
}

bool install() {
    std::lock_guard lock(installMutex);
    if (installed) return true;
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "GameHooks", "starting hook installation");
    hookSignature(SignatureId::VersionString, reinterpret_cast<void*>(versionDetour), &versionOriginal);
    const bool normalTickHooked = hookSignature(
        SignatureId::NormalTick, reinterpret_cast<void*>(tickDetour), &tickOriginal);
    gameHooksLogger().info(
        "NormalTick hook installed={}, signature={:p}.", normalTickHooked,
        reinterpret_cast<void*>(bedrocktools::memory::resolve(SignatureId::NormalTick)));
    const auto gameModeAttackEntry = bedrocktools::memory::resolve(SignatureId::GameModeAttack);
    const auto gameModeStartDestroyBlockEntry = bedrocktools::memory::resolve(SignatureId::GameModeStartDestroyBlock);
    const bool gameModeStartDestroyBlockCollision =
        gameModeStartDestroyBlockEntry != 0 && gameModeStartDestroyBlockEntry == gameModeAttackEntry;
    const bool gameModeStartDestroyBlockHooked = !gameModeStartDestroyBlockCollision && hookSignature(
        SignatureId::GameModeStartDestroyBlock, reinterpret_cast<void*>(gameModeStartDestroyBlockDetour), &gameModeStartDestroyBlockOriginal);
    if (gameModeStartDestroyBlockCollision)
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Error,
            "GameHooks", "start_destroy_block_hook", "skipped_attack_signature_collision");

    const auto survivalModeAttackEntry = bedrocktools::memory::resolve(SignatureId::SurvivalModeAttack);
    const auto survivalModeStartDestroyBlockEntry = bedrocktools::memory::resolve(SignatureId::SurvivalModeStartDestroyBlock);
    const bool survivalModeStartDestroyBlockCollision =
        survivalModeStartDestroyBlockEntry != 0 && survivalModeStartDestroyBlockEntry == survivalModeAttackEntry;
    const bool survivalModeStartDestroyBlockHooked = !survivalModeStartDestroyBlockCollision && hookSignature(
        SignatureId::SurvivalModeStartDestroyBlock, reinterpret_cast<void*>(survivalModeStartDestroyBlockDetour), &survivalModeStartDestroyBlockOriginal);
    if (survivalModeStartDestroyBlockCollision)
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Error,
            "GameHooks", "survival_start_destroy_block_hook", "skipped_attack_signature_collision");
    gameHooksLogger().info(
        "Block-start hooks: GameMode={}, SurvivalMode={}",
        gameModeStartDestroyBlockHooked, survivalModeStartDestroyBlockHooked);
    hookSignature(SignatureId::GameModeStopDestroyBlock, reinterpret_cast<void*>(gameModeStopDestroyBlockDetour), &gameModeStopDestroyBlockOriginal);
    hookSignature(SignatureId::GameModeStartBuildBlock, reinterpret_cast<void*>(gameModeStartBuildBlockDetour), &gameModeStartBuildBlockOriginal);
    hookSignature(SignatureId::SurvivalModeStartBuildBlock, reinterpret_cast<void*>(survivalModeStartBuildBlockDetour), &survivalModeStartBuildBlockOriginal);
    const bool gameModeUseItemHooked = hookSignature(
        SignatureId::GameModeUseItem, reinterpret_cast<void*>(gameModeUseItemDetour), &gameModeUseItemOriginal);
    const bool survivalModeUseItemHooked = hookSignature(
        SignatureId::SurvivalModeUseItem, reinterpret_cast<void*>(survivalModeUseItemDetour), &survivalModeUseItemOriginal);
    gameHooksLogger().info(
        "Consumption detection hooks: GameMode::useItem={}, SurvivalMode::useItem={}",
        gameModeUseItemHooked, survivalModeUseItemHooked);
    const bool gameModeUseItemAsAttackHooked = hookSignature(
        SignatureId::GameModeUseItemAsAttack, reinterpret_cast<void*>(gameModeUseItemAsAttackDetour), &gameModeUseItemAsAttackOriginal);
    const bool survivalUseItemAsAttackHooked = hookSignature(
        SignatureId::SurvivalModeUseItemAsAttack, reinterpret_cast<void*>(survivalModeUseItemAsAttackDetour), &survivalModeUseItemAsAttackOriginal);
    gameHooksLogger().info(
        "Touch activation hooks: GameMode::useItemAsAttack={}, SurvivalMode::useItemAsAttack={}",
        gameModeUseItemAsAttackHooked, survivalUseItemAsAttackHooked);
    hookSignature(SignatureId::GameModeUseItemOn, reinterpret_cast<void*>(gameModeUseItemOnDetour), &gameModeUseItemOnOriginal);
    hookSignature(SignatureId::SurvivalModeUseItemOn, reinterpret_cast<void*>(survivalModeUseItemOnDetour), &survivalModeUseItemOnOriginal);
    hookSignature(SignatureId::GameModeInteract, reinterpret_cast<void*>(gameModeInteractDetour), &gameModeInteractOriginal);
    hookSignature(SignatureId::SurvivalModeInteract, reinterpret_cast<void*>(survivalModeInteractDetour), &survivalModeInteractOriginal);
    const bool publicAttackSignature = bedrocktools::memory::resolve(SignatureId::GameModeAttack) != 0;
    const bool survivalAttackSignature = bedrocktools::memory::resolve(SignatureId::SurvivalModeAttack) != 0;
    const bool legacyInternalSignature = bedrocktools::memory::resolve(SignatureId::GameModeAttackInternal) != 0;
    gameHooksLogger().info(
        "Attack setup: legacy public-shaped signature={}, legacy survival signature={}, old internal pattern present={}, observer deferred until live public adapter validation.",
        publicAttackSignature, survivalAttackSignature, legacyInternalSignature);
    // Prefer the exact exported ClientInstance::update(bool) symbol when this
    // Bedrock build exports it; keep the signature scan as a version fallback.
    constexpr const char* clientUpdateSymbols[] = {
        "_ZN14ClientInstance6updateEb",
        "_ZN14ClientInstance7$updateEb"
    };
    std::uintptr_t clientUpdateAddress = 0;
    const char* clientUpdateSymbol = nullptr;
    for (const char* symbol : clientUpdateSymbols) {
        if (void* address = resolveMinecraftFunction(symbol)) {
            clientUpdateAddress = reinterpret_cast<std::uintptr_t>(address);
            clientUpdateSymbol = symbol;
            break;
        }
    }
    const char* clientUpdateSource = clientUpdateSymbol
        ? "exported_symbol" : "signature_pattern";
    if (!clientUpdateAddress)
        clientUpdateAddress = bedrocktools::memory::resolve(SignatureId::ClientInstanceUpdate);

    bool clientUpdateHooked = false;
    if (clientUpdateAddress && handleCount < handles.size()) {
        auto handle = bedrocktools::hooks::install(
            reinterpret_cast<void*>(clientUpdateAddress),
            reinterpret_cast<void*>(clientUpdateDetour),
            reinterpret_cast<void**>(&clientUpdateOriginal));
        if (handle) {
            handles[handleCount++] = handle;
            clientUpdateHooked = true;
        }
    }
    const std::string clientUpdateDetail =
        std::string("source=") + clientUpdateSource +
        "; target=" + std::to_string(clientUpdateAddress) +
        "; symbol=" + (clientUpdateSymbol ? clientUpdateSymbol : "unavailable");
    bedrocktools::diagnostics::probe(
        clientUpdateHooked ? bedrocktools::diagnostics::Level::Info
                           : bedrocktools::diagnostics::Level::Error,
        "GameHooks", "client_instance_update_hook",
        clientUpdateHooked
            ? (clientUpdateSymbol ? "installed_exported_symbol" : "installed_signature_pattern")
            : "install_failed",
        clientUpdateDetail);
    gameHooksLogger().info(
        "ClientInstance::update hook installed={}, source={}, target={:p}, symbol={}",
        clientUpdateHooked, clientUpdateSource,
        reinterpret_cast<void*>(clientUpdateAddress),
        clientUpdateSymbol ? clientUpdateSymbol : "unavailable");
    hookSignature(SignatureId::ContainerScreenControllerOpen, reinterpret_cast<void*>(containerOpenDetour), &containerOpenOriginal);
    hookSignature(SignatureId::ContainerScreenControllerDtor, reinterpret_cast<void*>(containerCloseDetour), &containerCloseOriginal);
    hookSignature(SignatureId::ChatScreenOpen, reinterpret_cast<void*>(chatOpenDetour), &chatOpenOriginal);
    hookSignature(SignatureId::ChatScreenDtor, reinterpret_cast<void*>(chatCloseDetour), &chatCloseOriginal);
    hookSignature(SignatureId::ContainerScreenControllerOnContainerSlotSelected, reinterpret_cast<void*>(containerSlotSelectedDetour), &containerSlotSelectedOriginal);
    const bool eglHooked = hookEgl();
    gameHooksLogger().info("Frame event hook installed={}; EGL swaps are always forwarded to the original renderer.", eglHooked);
    installed = tickOriginal != nullptr && clientUpdateOriginal != nullptr;
    bedrocktools::diagnostics::log(
        installed ? bedrocktools::diagnostics::Level::Info : bedrocktools::diagnostics::Level::Error,
        "GameHooks", installed ? "required tick/client hooks installed" : "required tick/client hooks missing");
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Debug, "GameHooks",
        "attack observer installation is deferred until a live public three-argument route is validated");
    return installed;
}

void uninstall() {
    std::lock_guard lock(installMutex);
    for (std::size_t i = 0; i < handleCount; ++i) {
        if (handles[i]) bedrocktools::hooks::remove(handles[i]);
        handles[i] = nullptr;
    }
    handleCount = 0;
    {
        std::lock_guard observerLock(attackObserverMutex);
        if (attackObserverHandle) bedrocktools::hooks::remove(attackObserverHandle);
        attackObserverHandle = nullptr;
        attackObserverTarget.store(nullptr, std::memory_order_release);
        gameModeAttackOriginal = nullptr;
    }
    installed = false;
    currentClientInstance.store(nullptr, std::memory_order_release);
    currentLocalPlayer.store(nullptr, std::memory_order_release);
    currentGameMode.store(nullptr, std::memory_order_release);
    playerSetGameTypeOriginal = nullptr;
    playerSetGameTypeHookInstalled.store(false, std::memory_order_release);
    playerSetGameTypeSeen.store(false, std::memory_order_release);
    playerSetGameTypeNextResolve = {};
    {
        std::lock_guard cacheLock(playerGameTypeCacheMutex);
        playerGameTypeCache.clear();
    }
}

void* clientInstance() {
    return currentClientInstance.load(std::memory_order_acquire);
}

void* localPlayer() {
    void* cached = currentLocalPlayer.load(std::memory_order_acquire);
    if (cached && isReadableMemory(cached, sizeof(void*))) return cached;
    void* client = currentClientInstance.load(std::memory_order_acquire);
    if (!client || !shouldResolveLocalPlayer(client)) return nullptr;
    return resolveLocalPlayerFromClientInstance(client);
}

void* gameMode() {
    return currentGameMode.load(std::memory_order_acquire);
}

void* gameModeForPlayer(void* player) {
    if (!player) return nullptr;

    // Prefer the receiver captured by a real engine callback, but require the
    // exact resolved public attack signature instead of merely a plausible game
    // vtable. This avoids accidentally retaining a neighboring polymorphic object.
    void* observed = currentGameMode.load(std::memory_order_acquire);
    // A receiver captured by one of our live GameMode detours is stronger
    // provenance than a guessed Player field. Keep it as the candidate when its
    // object/vtable still belongs to libminecraftpe; attackAvailable() performs
    // the stricter exact-attack check before any synthetic dispatch.
    if (observed && (publicAttackVirtualEntry(observed) != 0 ||
        publicUseItemVirtualEntry(observed, nullptr) != 0)) {
        return observed;
    }

    // Some game sessions have not yet produced a GameMode callback when native attack dispatch
    // first becomes active. The Player SDK already documents the owned mGameMode
    // field, but reading a version-sensitive offset is only acceptable after
    // validating the storage mapping and then matching the live vtable entry exactly to
    // the resolved GameMode/SurvivalMode attack signature. Any ambiguous pointer
    // is rejected; there is no fallback to an unverified native function.
    currentGameMode.store(nullptr, std::memory_order_release);
    const auto playerAddress = reinterpret_cast<std::uintptr_t>(player);
    constexpr auto gameModeOffset = bedrocktools::sdk::offsets::Player::mGameMode;
    if (playerAddress > std::numeric_limits<std::uintptr_t>::max() - gameModeOffset)
        return nullptr;
    const auto slotAddress = playerAddress + gameModeOffset;
    const auto* slot = reinterpret_cast<const void*>(slotAddress);
    if (!isReadableMemory(slot, sizeof(void*))) return nullptr;

    void* candidate = nullptr;
    std::memcpy(&candidate, slot, sizeof(candidate));
    auto isValidatedGameMode = [](void* value) {
        return value && (publicAttackVirtualEntry(value) != 0 ||
                         publicUseItemVirtualEntry(value, nullptr) != 0);
    };

    if (isValidatedGameMode(candidate)) {
        currentGameMode.store(candidate, std::memory_order_release);
        static std::atomic_bool loggedPlayerGameModeFallback{false};
        if (!loggedPlayerGameModeFallback.exchange(true, std::memory_order_acq_rel))
            gameHooksLogger().info("Recovered the Player-owned GameMode using exact public attack/useItem method validation.");
        return candidate;
    }

    // The SDK's mGameMode offset can drift between Bedrock client builds. As a
    // fallback, inspect pointer-aligned fields inside the local Player object,
    // but accept a candidate only if its live vtable contains an exact match for
    // a resolved GameMode/SurvivalMode attack or useItem entry in libminecraftpe.
    // This does not call any guessed address and never accepts a vtable merely
    // because it looks plausible. Limit the scan and rate-limit it per player so
    // an unsupported build does not add a per-tick scan cost.
    static thread_local const void* lastScannedPlayer = nullptr;
    static thread_local std::chrono::steady_clock::time_point nextScan{};
    const auto scanNow = std::chrono::steady_clock::now();
    if (lastScannedPlayer != player || scanNow >= nextScan) {
        lastScannedPlayer = player;
        nextScan = scanNow + std::chrono::milliseconds(750);
        constexpr std::size_t scanBegin = sizeof(void*);
        constexpr std::size_t scanEnd = 0x1800;
        for (std::size_t offset = scanBegin; offset + sizeof(void*) <= scanEnd;
             offset += alignof(void*)) {
            if (offset == gameModeOffset) continue;
            if (playerAddress > std::numeric_limits<std::uintptr_t>::max() - offset) break;
            const auto* field = reinterpret_cast<const void*>(playerAddress + offset);
            if (!isReadableMemory(field, sizeof(void*))) continue;
            void* possible = nullptr;
            std::memcpy(&possible, field, sizeof(possible));
            if (!isValidatedGameMode(possible)) continue;

            currentGameMode.store(possible, std::memory_order_release);
            gameHooksLogger().info(
                "Recovered Player-owned GameMode through exact vtable signature match at Player offset 0x{:X}.",
                offset);
            return possible;
        }
    }

    static std::atomic_bool loggedNoVerifiedGameMode{false};
    if (!loggedNoVerifiedGameMode.exchange(true, std::memory_order_acq_rel)) {
        const auto attackSignature = bedrocktools::memory::resolve(SignatureId::GameModeAttack);
        const auto useItemSignature = bedrocktools::memory::resolve(SignatureId::GameModeUseItem);
        const auto survivalAttackSignature = bedrocktools::memory::resolve(SignatureId::SurvivalModeAttack);
        const auto survivalUseItemSignature = bedrocktools::memory::resolve(SignatureId::SurvivalModeUseItem);
        gameHooksLogger().warn(
            "Could not validate Player-owned GameMode: player={:p}, observed={:p}, sdk_offset=0x{:X}, sdk_candidate={:p}, sdk_candidate_plausible={}, public_attack_sig={:p}, survival_attack_sig={:p}, public_useItem_sig={:p}, survival_useItem_sig={:p}. Bounded field scan found no exact trusted vtable match.",
            player, observed, static_cast<std::size_t>(gameModeOffset), candidate,
            plausibleGameMode(candidate), reinterpret_cast<void*>(attackSignature),
            reinterpret_cast<void*>(survivalAttackSignature),
            reinterpret_cast<void*>(useItemSignature),
            reinterpret_cast<void*>(survivalUseItemSignature));
        bedrocktools::diagnostics::probe(
            bedrocktools::diagnostics::Level::Warning, "GameHooks", "gamemode_resolution",
            "no_validated_player_owned_candidate",
            std::string("sdk_offset=0x") + [] (std::size_t value) {
                char buffer[32]{};
                std::snprintf(buffer, sizeof(buffer), "%zX", value);
                return std::string(buffer);
            }(static_cast<std::size_t>(gameModeOffset)));
    }
    return nullptr;
}

bool useItem(void* player, const void* itemStack) {
    if (!player || !itemStack) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "GameHooks", "use_item", "missing_player_or_stack");
        return false;
    }
    void* receiver = gameModeForPlayer(player);
    if (!receiver) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "GameHooks", "use_item", "gamemode_unavailable");
        return false;
    }
    std::size_t slotIndex = 0;
    if (!publicUseItemVirtualEntry(receiver, &slotIndex)) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "GameHooks", "use_item", "no_exact_public_vtable_match");
        return false;
    }
    void** vtable = nullptr;
    std::memcpy(&vtable, receiver, sizeof(vtable));
    if (!vtable) return false;
    const auto slotAddress = reinterpret_cast<std::uintptr_t>(vtable) + slotIndex * sizeof(void*);
    if (!isReadableMemory(reinterpret_cast<const void*>(slotAddress), sizeof(void*))) return false;
    auto nativeUse = reinterpret_cast<UseItemFn>(vtable[slotIndex]);
    if (!nativeUse) return false;

    const bool accepted = nativeUse(receiver, const_cast<void*>(itemStack));
    bedrocktools::diagnostics::probe(accepted ? bedrocktools::diagnostics::Level::Info :
            bedrocktools::diagnostics::Level::Warning,
        "GameHooks", "use_item", accepted ? "native_route_accepted" : "native_route_rejected",
        std::string("vtable_slot=") + std::to_string(slotIndex));
    return accepted;
}

bool attackAvailable(void* gameMode) {
    // Dispatch safety comes from validating the actual public GameMode vtable
    // entry against the resolved public signatures. The internal hook is only
    // used to observe the result/events; requiring it here silently disables
    // native attack dispatch on builds where that secondary signature is unavailable.
    if (!gameMode || currentGameMode.load(std::memory_order_acquire) != gameMode)
        return false;
    AttackKind ignored = AttackKind::GameMode;
    std::uintptr_t implementation = 0;
    if (!publicAttackVirtualEntry(gameMode, &ignored, nullptr, &implementation)) return false;
    if (implementation) (void)ensureAttackObserver(implementation);
    return true;
}

bool attack(void* gameMode, void* target, bool playPredictiveSound, const void* hitPosition) {
    if (!gameMode || !target || !hitPosition) return false;
    (void)playPredictiveSound;

    static thread_local bool inSyntheticAttack = false;
    if (inSyntheticAttack || currentGameMode.load(std::memory_order_acquire) != gameMode) return false;
    std::size_t matchedSlot = 0;
    AttackKind kind = AttackKind::GameMode;
    std::uintptr_t implementation = 0;
    const auto publicEntry = publicAttackVirtualEntry(gameMode, &kind, &matchedSlot, &implementation);
    if (!publicEntry) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "GameHooks", "attack", "public_three_argument_route_unavailable");
        return false;
    }
    if (implementation) (void)ensureAttackObserver(implementation);
    void** vtable = nullptr;
    std::memcpy(&vtable, gameMode, sizeof(vtable));
    const auto slotAddress = reinterpret_cast<std::uintptr_t>(vtable) + matchedSlot * sizeof(void*);
    if (!vtable || !isReadableMemory(reinterpret_cast<const void*>(slotAddress), sizeof(void*))) return false;
    auto publicAttack = reinterpret_cast<PublicAttackFn>(vtable[matchedSlot]);
    if (!publicAttack) return false;

    struct AttackGuard { bool& active; explicit AttackGuard(bool& v) : active(v) { active = true; } ~AttackGuard() { active = false; } } attackGuard{inSyntheticAttack};

    // Publish events once at the synthetic caller. Some runtimes do not pass
    // through the old internal observer after invoking the public virtual.
    const GameModeType modeKind = kind == AttackKind::SurvivalMode ? GameModeType::SurvivalMode : GameModeType::GameMode;
    GameModeActionEvent actionEvent{modeKind, GameModeAction::Attack, gameMode};
    bus().publish(actionEvent);
    AttackEvent attackEvent{kind, gameMode, reinterpret_cast<bedrocktools::sdk::Actor*>(target),
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(true)), const_cast<void*>(hitPosition)};
    bus().publish(attackEvent);
    if (attackEvent.cancelled()) {
        AttackResultEvent cancelled{kind, reinterpret_cast<bedrocktools::sdk::Actor*>(target), false};
        bus().publish(cancelled);
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
            "GameHooks", "attack", "cancelled_by_attack_event");
        return false;
    }

    const bool observe = gameModeAttackOriginal != nullptr;
    struct ObservationGuard {
        bool active;
        explicit ObservationGuard(bool value) : active(value) {
            if (active) { syntheticAttackResultObserved = false; syntheticAttackNativeResult = false; syntheticAttackDispatchActive = true; }
        }
        ~ObservationGuard() { if (active) syntheticAttackDispatchActive = false; }
    } observation{observe};
    struct PrepublishedGuard {
        bool previous;
        explicit PrepublishedGuard(bool& value) : previous(value) { value = true; }
        ~PrepublishedGuard() { syntheticAttackEventsPrepublished = previous; }
    } prepublished{syntheticAttackEventsPrepublished};

    static std::atomic_bool loggedPublic{false};
    if (!loggedPublic.exchange(true, std::memory_order_acq_rel)) {
        char msg[144]{};
        std::snprintf(msg, sizeof(msg), "dispatching public three-argument attack virtual at slot %zu", matchedSlot);
        gameHooksLogger().info("{}", msg);
    }
    // Correct public ABI is (this, Actor*, Vec3 const*), without bool.
    const bool publicResult = publicAttack(gameMode, target, hitPosition);
    const bool accepted = syntheticAttackResultObserved ? syntheticAttackNativeResult : publicResult;
    AttackResultEvent resultEvent{kind, reinterpret_cast<bedrocktools::sdk::Actor*>(target), accepted};
    bus().publish(resultEvent);
    bedrocktools::diagnostics::probe(accepted ? bedrocktools::diagnostics::Level::Info : bedrocktools::diagnostics::Level::Warning,
        "GameHooks", "attack", accepted ? "public_virtual_returned_true" : "public_virtual_returned_false",
        std::string("vtable_slot=") + std::to_string(matchedSlot) +
        "; internal_observer=" + (syntheticAttackResultObserved ? "observed" : "not_reached") +
        "; return_true_does_not_prove_server_damage");
    return accepted;
}


bool tryGetPlayerGameType(void* player, std::int32_t& outGameType) {
    if (!player) return false;
    if (!playerGetGameTypeOriginal &&
        std::chrono::steady_clock::now() >= playerGameTypeNextResolve) {
        // Retry the exported getter and setter observer at a bounded cadence. The
        // resolver owns the separate retry schedule for the setter hook.
        resolvePlayerGameTypeApi();
    }

    if (playerGetGameTypeOriginal) {
        const std::int32_t nativeValue = playerGetGameTypeOriginal(player);
        std::int32_t canonical = -1;
        // Native C++ GameType values: Survival=0, Creative=1, Adventure=2,
        // Default=5, Spectator=6. Undefined and Default are not player modes.
        if (bedaim::playergametype::normalizeNativeValue(nativeValue, canonical)) {
            outGameType = canonical;
            return true;
        }
        static std::atomic_bool loggedUnknownNativeGameType{false};
        if (!loggedUnknownNativeGameType.exchange(true, std::memory_order_acq_rel)) {
            gameHooksLogger().warn(
                "Player::getPlayerGameType returned unrecognized native value {}; checking the observed setter cache instead of guessing.",
                nativeValue);
        }
    }

    {
        std::lock_guard<std::mutex> cacheLock(playerGameTypeCacheMutex);
        const auto cached = playerGameTypeCache.find(player);
        if (cached != playerGameTypeCache.end()) {
            outGameType = cached->second;
            return true;
        }
    }

    static std::atomic_bool loggedMissingGameType{false};
    if (!loggedMissingGameType.exchange(true, std::memory_order_acq_rel)) {
        gameHooksLogger().warn(
            "No native GameType getter result or setter-cache entry is available yet (getter={}, setterHook={}, setterObserved={}); this player's mode remains Unknown until the engine updates a cached player or a matching-build packet hook is added.",
            playerGetGameTypeOriginal != nullptr,
            playerSetGameTypeHookInstalled.load(std::memory_order_acquire),
            playerSetGameTypeSeen.load(std::memory_order_acquire));
    }
    return false;
}


}
