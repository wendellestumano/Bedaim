#include "GameHooks.hpp"
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
using PublicAttackFn = bool(*)(void*, void*, bool, const void*);
using ClientInstanceUpdateFn = void*(*)(void*, bool);
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
std::array<bedrocktools::hooks::Handle, 26> handles{};
std::size_t handleCount = 0;
std::mutex installMutex;
bool installed = false;
std::string gameVersion;
thread_local std::uint32_t gameModeActionDepth = 0;
thread_local bool syntheticAttackDispatchActive = false;
thread_local bool syntheticAttackResultObserved = false;
thread_local bool syntheticAttackNativeResult = false;
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
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
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

bool plausibleGameMode(void* gameMode) {
    if (!gameMode || !isReadableMemory(gameMode, sizeof(void*))) return false;
    void** vtable = nullptr;
    std::memcpy(&vtable, gameMode, sizeof(vtable));
    if (!vtable || !isReadableMemory(vtable, 8 * sizeof(void*))) return false;
    void* first = nullptr;
    std::memcpy(&first, vtable, sizeof(first));
    return first && isMinecraftFunction(first);
}

// Find the exact public wrapper in the object's live vtable rather than
// assuming every patched Bedrock build keeps GameMode::attack at index 16.
std::uintptr_t publicAttackVirtualEntry(void* gameMode, AttackKind* kind = nullptr,
                                        std::size_t* matchedSlot = nullptr) {
    if (!plausibleGameMode(gameMode)) return 0;
    void** vtable = nullptr;
    std::memcpy(&vtable, gameMode, sizeof(vtable));
    const auto baseSignature = bedrocktools::memory::resolve(SignatureId::GameModeAttack);
    const auto survivalSignature = bedrocktools::memory::resolve(SignatureId::SurvivalModeAttack);
    struct Exported { std::uintptr_t base{}; std::uintptr_t survival{}; };
    static const Exported symbols = [] {
        Exported out{};
        constexpr const char* baseNames[] = {
            "_ZN8GameMode6attackEP5ActorbRK4Vec3", "_ZN8GameMode6attackER5ActorbRK4Vec3",
            "_ZN8GameMode7$attackEP5ActorbRK4Vec3", "_ZN8GameMode7$attackER5ActorbRK4Vec3"
        };
        constexpr const char* survivalNames[] = {
            "_ZN12SurvivalMode6attackEP5ActorbRK4Vec3", "_ZN12SurvivalMode6attackER5ActorbRK4Vec3",
            "_ZN12SurvivalMode7$attackEP5ActorbRK4Vec3", "_ZN12SurvivalMode7$attackER5ActorbRK4Vec3"
        };
        for (const char* name : baseNames) if (void* p = resolveMinecraftFunction(name)) { out.base = reinterpret_cast<std::uintptr_t>(p); break; }
        for (const char* name : survivalNames) if (void* p = resolveMinecraftFunction(name)) { out.survival = reinterpret_cast<std::uintptr_t>(p); break; }
        return out;
    }();
    for (std::size_t i = 0; i < 64; ++i) {
        const auto* slot = reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(vtable) + i * sizeof(void*));
        if (!isReadableMemory(slot, sizeof(void*))) break;
        std::uintptr_t address = 0;
        std::memcpy(&address, slot, sizeof(address));
        if (!address) break;
        AttackKind found = AttackKind::GameMode;
        bool match = false;
        const auto canonicalAddress = canonicalArm64Entry(address);
        const bool survivalMatch =
            (survivalSignature && (address == survivalSignature ||
                canonicalAddress == canonicalArm64Entry(survivalSignature))) ||
            (symbols.survival && (address == symbols.survival ||
                canonicalAddress == canonicalArm64Entry(symbols.survival)));
        const bool baseMatch =
            (baseSignature && (address == baseSignature ||
                canonicalAddress == canonicalArm64Entry(baseSignature))) ||
            (symbols.base && (address == symbols.base ||
                canonicalAddress == canonicalArm64Entry(symbols.base)));
        if (survivalMatch) {
            found = AttackKind::SurvivalMode; match = true;
        } else if (baseMatch) {
            found = AttackKind::GameMode; match = true;
        }
        if (!match) continue;
        if (kind) *kind = found;
        if (matchedSlot) *matchedSlot = i;
        static std::atomic_bool loggedMatch{false};
        if (!loggedMatch.exchange(true, std::memory_order_acq_rel))
            gameHooksLogger().info("Validated public attack through exact vtable match at slot {}.", i);
        return address;
    }
    static std::atomic_bool loggedMiss{false};
    if (!loggedMiss.exchange(true, std::memory_order_acq_rel)) {
        std::uintptr_t slot0 = 0, slot2 = 0, slot16 = 0, slot32 = 0;
        const auto readSlot = [&](std::size_t index, std::uintptr_t& out) {
            const auto address = reinterpret_cast<std::uintptr_t>(vtable) + index * sizeof(void*);
            if (isReadableMemory(reinterpret_cast<const void*>(address), sizeof(void*)))
                std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(out));
        };
        readSlot(0, slot0); readSlot(2, slot2); readSlot(16, slot16); readSlot(32, slot32);
        gameHooksLogger().warn(
            "No exact public GameMode::attack vtable match: object={:p}, vtable={:p}, public_sig={:p}, survival_sig={:p}, slot0={:p}, slot2={:p}, slot16={:p}, slot32={:p}.",
            gameMode, reinterpret_cast<const void*>(vtable), reinterpret_cast<void*>(baseSignature),
            reinterpret_cast<void*>(survivalSignature), reinterpret_cast<void*>(slot0),
            reinterpret_cast<void*>(slot2), reinterpret_cast<void*>(slot16), reinterpret_cast<void*>(slot32));
    }
    return 0;
}

std::uintptr_t publicUseItemVirtualEntry(void* gameMode, std::size_t* matchedSlot = nullptr) {
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
    for (std::size_t i = 0; i < 64; ++i) {
        const auto* slot = reinterpret_cast<const void*>(
            reinterpret_cast<std::uintptr_t>(vtable) + i * sizeof(void*));
        if (!isReadableMemory(slot, sizeof(void*))) break;
        std::uintptr_t address = 0;
        std::memcpy(&address, slot, sizeof(address));
        if (!address) break;
        if ((baseSignature && address == baseSignature) || (symbols.base && address == symbols.base) ||
            (survivalSignature && address == survivalSignature) || (symbols.survival && address == symbols.survival)) {
            if (matchedSlot) *matchedSlot = i;
            static std::atomic_bool loggedMatch{false};
            if (!loggedMatch.exchange(true, std::memory_order_acq_rel))
                gameHooksLogger().info("Validated public useItem through exact vtable match at slot {}.", i);
            return address;
        }
    }
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
    if (!vtable || !isReadableMemory(vtable, sizeof(void*))) return false;

    static const auto normalTick = bedrocktools::memory::resolve(SignatureId::NormalTick);
    if (normalTick == 0 || !isMinecraftFunction(reinterpret_cast<const void*>(normalTick)) ||
        !isReadableMemory(vtable, sizeof(void*) * 64)) return false;

    // On the 1.26.51.01 binary, the unique NormalTick signature is the
    // LocalPlayer vtable entry. Verify the actor by identity instead of calling
    // ClientInstance's incompatible getter ABI as if it returned Player*.
    for (std::size_t i = 0; i < 64; ++i) {
        std::uintptr_t entry = 0;
        std::memcpy(&entry, vtable + i, sizeof(entry));
        if (entry == normalTick) return true;
    }
    return false;
}

void* resolveLocalPlayerFromClientInstance(void* clientInstance) {
    (void)clientInstance;
    static std::atomic_bool loggedUnavailable{false};
    void* player = currentLocalPlayer.load(std::memory_order_acquire);
    if (!player || !isReadableMemory(player, sizeof(void*))) {
        if (!loggedUnavailable.exchange(true, std::memory_order_acq_rel))
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
                "GameHooks", "verified_local_player", "normal_tick_capture_unavailable");
        return nullptr;
    }
    return player;
}

void tickDetour(void* actor) {
    // Raw actor events remain available to modules that need them. Combat and
    // inventory modules receive only the actor verified against LocalPlayer::NormalTick.
    auto* actorPlayer = reinterpret_cast<bedrocktools::sdk::Player*>(actor);
    const bool actorIsLocalPlayer = isLocalPlayerTickObject(actor);
    if (actorIsLocalPlayer) {
        currentLocalPlayer.store(actor, std::memory_order_release);
        static std::atomic_bool loggedCapture{false};
        if (!loggedCapture.exchange(true, std::memory_order_acq_rel))
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
                "GameHooks", "verified_local_player", "captured_from_normal_tick_vtable");
    }

    void* localPlayer = actorIsLocalPlayer ? actor : resolveLocalPlayerFromClientInstance(
        currentClientInstance.load(std::memory_order_acquire));
    const bool isVerifiedLocal = actor && localPlayer && actor == localPlayer;

    LocalPlayerPreTickEvent preEvent{actorPlayer};
    bus().publish(preEvent);
    if (isVerifiedLocal) {
        VerifiedLocalPlayerPreTickEvent verifiedEvent{
            reinterpret_cast<bedrocktools::sdk::Player*>(localPlayer)};
        bus().publish(verifiedEvent);
    }
    if (tickOriginal) tickOriginal(actor);
    LocalPlayerTickEvent event{actorPlayer};
    bus().publish(event);
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

    const AttackKind kind = attackKindFor(gameMode);
    const GameModeType modeKind = kind == AttackKind::SurvivalMode ? GameModeType::SurvivalMode : GameModeType::GameMode;
    GameModeActionScope scope{modeKind, gameMode};
    scope.publish(GameModeAction::Attack);
    AttackEvent event{kind, gameMode, reinterpret_cast<bedrocktools::sdk::Actor*>(target), reinterpret_cast<void*>(static_cast<std::uintptr_t>(playPredictiveSound)), const_cast<void*>(hitPosition)};
    bus().publish(event);
    if (event.cancelled()) {
        if (syntheticAttackDispatchActive) {
            syntheticAttackResultObserved = true;
            syntheticAttackNativeResult = false;
        }
        static std::atomic_bool loggedCancelled{false};
        if (!loggedCancelled.exchange(true, std::memory_order_acq_rel))
            gameHooksLogger().warn("The first native attack was cancelled by an AttackEvent listener.");
        return false;
    }
    const bool result = gameModeAttackOriginal
        ? gameModeAttackOriginal(gameMode, target, playPredictiveSound, hitPosition)
        : false;
    if (syntheticAttackDispatchActive) {
        syntheticAttackResultObserved = true;
        syntheticAttackNativeResult = result;
    }
    static std::atomic_bool loggedNativeResult{false};
    if (!loggedNativeResult.exchange(true, std::memory_order_acq_rel))
        gameHooksLogger().info("First native attack trampoline result: {}.", result);
    // Publish after native processing so the HUD can react to the actual attack result.
    AttackResultEvent resultEvent{kind, reinterpret_cast<bedrocktools::sdk::Actor*>(target), result};
    bus().publish(resultEvent);
    return result;
}

void* clientUpdateDetour(void* clientInstance, bool value) {
    void* previousClientInstance = currentClientInstance.exchange(clientInstance, std::memory_order_acq_rel);
    if (previousClientInstance != clientInstance)
        currentLocalPlayer.store(nullptr, std::memory_order_release);
    void* result = clientUpdateOriginal ? clientUpdateOriginal(clientInstance, value) : nullptr;
    ClientInstanceUpdateEvent event{reinterpret_cast<bedrocktools::sdk::ClientInstance*>(clientInstance)};
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
    const bool internalAttackHooked = hookSignature(
        SignatureId::GameModeAttackInternal, reinterpret_cast<void*>(gameModeAttackDetour), &gameModeAttackOriginal);
    const bool publicAttackSignature = bedrocktools::memory::resolve(SignatureId::GameModeAttack) != 0;
    const bool survivalAttackSignature = bedrocktools::memory::resolve(SignatureId::SurvivalModeAttack) != 0;
    const bool internalAttackSignature = bedrocktools::memory::resolve(SignatureId::GameModeAttackInternal) != 0;
    gameHooksLogger().info(
        "Attack setup: public signature={}, survival signature={}, internal signature={}, internal hook={}, trampoline={}",
        publicAttackSignature, survivalAttackSignature, internalAttackSignature,
        internalAttackHooked, gameModeAttackOriginal != nullptr);
    const bool clientUpdateHooked = hookSignature(
        SignatureId::ClientInstanceUpdate, reinterpret_cast<void*>(clientUpdateDetour), &clientUpdateOriginal);
    gameHooksLogger().info(
        "ClientInstance capture hook installed={}, signature={:p}.", clientUpdateHooked,
        reinterpret_cast<void*>(bedrocktools::memory::resolve(SignatureId::ClientInstanceUpdate)));
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
        std::string("attack hook available: ") + (gameModeAttackOriginal ? "true" : "false"));
    return installed;
}

void uninstall() {
    std::lock_guard lock(installMutex);
    for (std::size_t i = 0; i < handleCount; ++i) {
        if (handles[i]) bedrocktools::hooks::remove(handles[i]);
        handles[i] = nullptr;
    }
    handleCount = 0;
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
    return resolveLocalPlayerFromClientInstance(
        currentClientInstance.load(std::memory_order_acquire));
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
        publicUseItemVirtualEntry(observed) != 0)) {
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
                         publicUseItemVirtualEntry(value) != 0);
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
    return publicAttackVirtualEntry(gameMode, &ignored) != 0;
}

bool attack(void* gameMode, void* target, bool playPredictiveSound, const void* hitPosition) {
    if (!gameMode || !target || !hitPosition) return false;
    (void)playPredictiveSound; // The validated public wrapper owns this default.

    static thread_local bool inSyntheticAttack = false;
    if (inSyntheticAttack) return false;
    if (currentGameMode.load(std::memory_order_acquire) != gameMode) return false;

    // GameMode::attack has a four-argument virtual ABI on this runtime:
    // (GameMode*, Actor*, predictive-sound flag, hit position). Only dispatch
    // through a vtable entry that matches a resolved attack signature.
    std::size_t matchedAttackSlot = 0;
    const auto publicEntry = publicAttackVirtualEntry(gameMode, nullptr, &matchedAttackSlot);
    if (publicEntry == 0) {
        static std::atomic_bool loggedUnavailable{false};
        if (!loggedUnavailable.exchange(true, std::memory_order_acq_rel)) {
            if (!plausibleGameMode(gameMode)) {
                bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error,
                    "GameHooks",
                    "attack_validation=implausible_gamemode: object/vtable did not pass the libminecraftpe validation");
            } else {
                void** vtable = nullptr;
                std::memcpy(&vtable, gameMode, sizeof(vtable));
                std::uintptr_t attackVtableEntry = 0;
                std::uintptr_t legacyVtableEntry = 0;
                const auto attackSlot = reinterpret_cast<std::uintptr_t>(vtable) + 2 * sizeof(void*);
                const auto legacySlot = reinterpret_cast<std::uintptr_t>(vtable) + 16 * sizeof(void*);
                if (vtable && isReadableMemory(reinterpret_cast<const void*>(attackSlot), sizeof(void*)))
                    std::memcpy(&attackVtableEntry, reinterpret_cast<const void*>(attackSlot), sizeof(attackVtableEntry));
                if (vtable && isReadableMemory(reinterpret_cast<const void*>(legacySlot), sizeof(void*)))
                    std::memcpy(&legacyVtableEntry, reinterpret_cast<const void*>(legacySlot), sizeof(legacyVtableEntry));
                const auto publicSignature = bedrocktools::memory::resolve(SignatureId::GameModeAttack);
                const auto survivalSignature = bedrocktools::memory::resolve(SignatureId::SurvivalModeAttack);
                char detail[320]{};
                std::snprintf(detail, sizeof(detail),
                    "attack_validation=vtable_mismatch: slot2=%p slot16=%p public_signature=%p survival_signature=%p",
                    reinterpret_cast<void*>(attackVtableEntry),
                    reinterpret_cast<void*>(legacyVtableEntry),
                    reinterpret_cast<void*>(publicSignature),
                    reinterpret_cast<void*>(survivalSignature));
                bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error,
                    "GameHooks", detail);
            }
            gameHooksLogger().error("Synthetic attack refused: public GameMode::attack is not validated for this runtime.");
        }
        return false;
    }
    void** vtable = nullptr;
    std::memcpy(&vtable, gameMode, sizeof(vtable));
    const auto slotAddress = reinterpret_cast<std::uintptr_t>(vtable) + matchedAttackSlot * sizeof(void*);
    if (!vtable || !isReadableMemory(reinterpret_cast<const void*>(slotAddress), sizeof(void*))) return false;
    auto publicAttack = reinterpret_cast<PublicAttackFn>(vtable[matchedAttackSlot]);
    if (!publicAttack) return false;

    struct AttackGuard {
        bool& active;
        explicit AttackGuard(bool& value) : active(value) { active = true; }
        ~AttackGuard() { active = false; }
    } guard{inSyntheticAttack};
    const bool canObserveNativeResult = gameModeAttackOriginal != nullptr;
    struct DispatchObservationGuard {
        bool active;
        explicit DispatchObservationGuard(bool shouldObserve) : active(shouldObserve) {
            if (active) {
                syntheticAttackResultObserved = false;
                syntheticAttackNativeResult = false;
                syntheticAttackDispatchActive = true;
            }
        }
        ~DispatchObservationGuard() {
            if (active) syntheticAttackDispatchActive = false;
        }
    } observationGuard{canObserveNativeResult};

    static std::atomic_bool loggedPublicRoute{false};
    if (!loggedPublicRoute.exchange(true, std::memory_order_acq_rel))
        gameHooksLogger().info("Combat dispatch selected the validated public GameMode::attack vtable entry.");

    // Dispatch with the native four-argument ABI. When the internal observer is
    // installed, return its actual result; otherwise preserve the public method's
    // return value instead of claiming an unverified successful attack.
    const bool publicResult = publicAttack(gameMode, target, playPredictiveSound, hitPosition);
    if (!canObserveNativeResult) {
        // The call went through a validated public engine entry, but this build
        // lacks the internal observer required to read the native bool result.
        // Fall back to the public method's native return value. It indicates
        // whether the engine accepted the attack, not guaranteed target damage.
        static std::atomic_bool loggedUnobservedDispatch{false};
        if (!loggedUnobservedDispatch.exchange(true, std::memory_order_acq_rel))
            gameHooksLogger().warn("Native attack used the validated public GameMode::attack entry, but the internal result hook is unavailable; using the public return value.");
        return publicResult;
    }
    if (!syntheticAttackResultObserved) {
        static std::atomic_bool loggedMissingInternalDispatch{false};
        if (!loggedMissingInternalDispatch.exchange(true, std::memory_order_acq_rel))
            gameHooksLogger().error("Validated public attack returned without reaching the installed internal observer; using the public return value.");
        return publicResult;
    }
    return syntheticAttackNativeResult;
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
