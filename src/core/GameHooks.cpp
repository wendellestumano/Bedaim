#include "GameHooks.hpp"
#include "Diagnostics.hpp"
#include "modules/player/PlayerGameType.hpp"
#include <bedrocktools/Version.hpp>

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/AttackResultEvent.hpp>
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
using PublicAttackFn = void(*)(void*, void*, const void*);
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
using ActorIsAttackableGamemodeFn = bool (*)(void*);
PlayerGetGameTypeFn playerGetGameTypeOriginal = nullptr;
std::atomic<ActorIsAttackableGamemodeFn> actorIsAttackableGamemodeOriginal{nullptr};
std::chrono::steady_clock::time_point actorAttackabilityNextResolve{};
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

bool tryIsActorAttackableGamemode(void* actor, bool& outIsAttackable) {
    if (!actor) return false;
    auto function = actorIsAttackableGamemodeOriginal.load(std::memory_order_acquire);
    if (!function && std::chrono::steady_clock::now() >= actorAttackabilityNextResolve) {
        std::lock_guard<std::mutex> cacheLock(playerGameTypeCacheMutex);
        const auto now = std::chrono::steady_clock::now();
        function = actorIsAttackableGamemodeOriginal.load(std::memory_order_relaxed);
        if (!function && now >= actorAttackabilityNextResolve) {
            actorAttackabilityNextResolve = now + std::chrono::seconds(1);
            // Apollon calls Actor::isAttackableGamemode() directly. Prefer the
            // matching exported native method over inferring attackability from
            // GameType IDs. Both const/non-const manglings have the same ABI here;
            // resolveMinecraftFunction validates that the address belongs to the
            // loaded Minecraft library. Do not fall back to unverified offsets.
            const char* symbols[] = {
                "_ZN5Actor20isAttackableGamemodeEv",
                "_ZNK5Actor20isAttackableGamemodeEv"
            };
            for (const char* symbol : symbols) {
                auto* address = resolveMinecraftFunction(symbol);
                if (!address) continue;
                function = reinterpret_cast<ActorIsAttackableGamemodeFn>(address);
                actorIsAttackableGamemodeOriginal.store(function, std::memory_order_release);
                gameHooksLogger().info(
                    "Resolved trusted Actor::isAttackableGamemode; Aim Assist can apply the native attackability filter.");
                break;
            }
        }
    }
    if (!function) return false;
    outIsAttackable = function(actor);
    return true;
}

bool plausibleGameMode(void* gameMode) {
    if (!gameMode) return false;
    static std::atomic<void*> lastValidatedGameMode{nullptr};

    // A pointer validated once is not guaranteed to remain alive after respawn,
    // world unload, or GameMode replacement. Re-check mappings and its vtable
    // before every native call; neither success nor failure is cached as truth.
    constexpr std::size_t kKnownAttackSlot = 16;
    if (!isReadableMemory(gameMode, sizeof(void*))) {
        lastValidatedGameMode.store(nullptr, std::memory_order_release);
        return false;
    }
    auto** vtable = *reinterpret_cast<void***>(gameMode);
    if (!vtable || !isReadableMemory(vtable, (kKnownAttackSlot + 1) * sizeof(void*)) ||
        !vtable[kKnownAttackSlot]) {
        lastValidatedGameMode.store(nullptr, std::memory_order_release);
        return false;
    }

    Dl_info info{};
    if (!dladdr(vtable[kKnownAttackSlot], &info) || !info.dli_fname ||
        std::string_view(info.dli_fname).find("libminecraftpe.so") == std::string_view::npos) {
        lastValidatedGameMode.store(nullptr, std::memory_order_release);
        return false;
    }

    lastValidatedGameMode.store(gameMode, std::memory_order_release);
    return true;
}

// GameMode::attack is the public wrapper at vtable slot 16. Its validated
// signature is a 3-argument wrapper (this, target, hit-position) which prepares
// the internal 4-argument call itself. Never call the internal body directly:
// doing so bypasses that setup and previously caused the game to stall.
std::uintptr_t publicAttackVirtualEntry(void* gameMode, AttackKind* kind = nullptr) {
    if (!plausibleGameMode(gameMode)) return 0;
    auto** vtable = *reinterpret_cast<void***>(gameMode);
    constexpr std::size_t kKnownAttackSlot = 16;
    const auto slotAddress = reinterpret_cast<std::uintptr_t>(vtable[kKnownAttackSlot]);
    const auto publicGameModeAttack = bedrocktools::memory::resolve(SignatureId::GameModeAttack);
    const auto publicSurvivalAttack = bedrocktools::memory::resolve(SignatureId::SurvivalModeAttack);

    if (publicSurvivalAttack && slotAddress == publicSurvivalAttack) {
        if (kind) *kind = AttackKind::SurvivalMode;
        return slotAddress;
    }
    if (publicGameModeAttack && slotAddress == publicGameModeAttack) {
        if (kind) *kind = AttackKind::GameMode;
        return slotAddress;
    }

    // Signature patterns can resolve the internal attack body on builds where
    // the public virtual wrapper has a different prologue. Prefer exact exported
    // C++ method symbols as a fallback, but still require the live vtable slot to
    // equal the resolved symbol before dispatching through it.
    struct PublicAttackSymbols {
        std::uintptr_t gameMode{};
        std::uintptr_t survivalMode{};
    };
    static const PublicAttackSymbols exportedSymbols = [] {
        PublicAttackSymbols result{};
        constexpr const char* gameModeNames[] = {
            "_ZN8GameMode6attackEP5ActorRK4Vec3",
            "_ZN8GameMode6attackER5ActorRK4Vec3"
        };
        constexpr const char* survivalModeNames[] = {
            "_ZN12SurvivalMode6attackEP5ActorRK4Vec3",
            "_ZN12SurvivalMode6attackER5ActorRK4Vec3"
        };
        for (const char* name : gameModeNames) {
            if (void* address = resolveMinecraftFunction(name)) {
                result.gameMode = reinterpret_cast<std::uintptr_t>(address);
                break;
            }
        }
        for (const char* name : survivalModeNames) {
            if (void* address = resolveMinecraftFunction(name)) {
                result.survivalMode = reinterpret_cast<std::uintptr_t>(address);
                break;
            }
        }
        return result;
    }();
    if (exportedSymbols.survivalMode && slotAddress == exportedSymbols.survivalMode) {
        if (kind) *kind = AttackKind::SurvivalMode;
        static std::atomic_bool loggedSymbolFallback{false};
        if (!loggedSymbolFallback.exchange(true, std::memory_order_acq_rel))
            gameHooksLogger().info("Validated public attack wrapper via exact exported SurvivalMode::attack symbol.");
        return slotAddress;
    }
    if (exportedSymbols.gameMode && slotAddress == exportedSymbols.gameMode) {
        if (kind) *kind = AttackKind::GameMode;
        static std::atomic_bool loggedSymbolFallback{false};
        if (!loggedSymbolFallback.exchange(true, std::memory_order_acq_rel))
            gameHooksLogger().info("Validated public attack wrapper via exact exported GameMode::attack symbol.");
        return slotAddress;
    }
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

void tickDetour(void* actor) {
    // The known-good BedrockTools 1.5.7 package publishes the NormalTick
    // actor directly. Requiring a separate ClientInstance getter here
    // suppresses both player-tick events when that unrelated getter fails.
    auto* player = reinterpret_cast<bedrocktools::sdk::Player*>(actor);
    LocalPlayerPreTickEvent preEvent{player};
    bus().publish(preEvent);
    if (tickOriginal) tickOriginal(actor);
    LocalPlayerTickEvent event{player};
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
    if (clientInstance) currentClientInstance.store(clientInstance, std::memory_order_release);
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
    hookSignature(SignatureId::NormalTick, reinterpret_cast<void*>(tickDetour), &tickOriginal);
    hookSignature(SignatureId::GameModeStartDestroyBlock, reinterpret_cast<void*>(gameModeStartDestroyBlockDetour), &gameModeStartDestroyBlockOriginal);
    hookSignature(SignatureId::SurvivalModeStartDestroyBlock, reinterpret_cast<void*>(survivalModeStartDestroyBlockDetour), &survivalModeStartDestroyBlockOriginal);
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
    hookSignature(SignatureId::ClientInstanceUpdate, reinterpret_cast<void*>(clientUpdateDetour), &clientUpdateOriginal);
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
    currentGameMode.store(nullptr, std::memory_order_release);
    playerSetGameTypeOriginal = nullptr;
    actorIsAttackableGamemodeOriginal.store(nullptr, std::memory_order_release);
    actorAttackabilityNextResolve = {};
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

void* gameMode() {
    return currentGameMode.load(std::memory_order_acquire);
}

void* gameModeForPlayer(void* player) {
    if (!player) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "GameHooks", "game_mode_resolution", "missing_player");
        return nullptr;
    }

    // Prefer the receiver captured by a real engine callback, but require the
    // exact resolved public attack signature instead of merely a plausible game
    // vtable. This avoids accidentally retaining a neighboring polymorphic object.
    void* observed = currentGameMode.load(std::memory_order_acquire);
    if (observed) {
        if (publicAttackVirtualEntry(observed) != 0) {
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
                "GameHooks", "game_mode_resolution", "ready_from_observed_action");
            return observed;
        }
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "GameHooks", "game_mode_resolution", "observed_instance_unverified",
            "observed action receiver did not match the public attack wrapper");
    }

    // Some game sessions have not yet produced a GameMode callback when native attack dispatch
    // first becomes active. The Player SDK already documents the owned mGameMode
    // field, but reading a version-sensitive offset is only acceptable after
    // validating the storage mapping and then matching vtable slot 16 exactly to
    // the resolved GameMode/SurvivalMode attack signature. Any ambiguous pointer
    // is rejected; there is no fallback to an unverified native function.
    currentGameMode.store(nullptr, std::memory_order_release);
    const auto playerAddress = reinterpret_cast<std::uintptr_t>(player);
    constexpr auto gameModeOffset = bedrocktools::sdk::offsets::Player::mGameMode;
    if (playerAddress > std::numeric_limits<std::uintptr_t>::max() - gameModeOffset) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Error,
            "GameHooks", "game_mode_resolution", "player_field_address_overflow");
        return nullptr;
    }
    const auto slotAddress = playerAddress + gameModeOffset;
    const auto* slot = reinterpret_cast<const void*>(slotAddress);
    if (!isReadableMemory(slot, sizeof(void*))) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "GameHooks", "game_mode_resolution", "player_field_unreadable",
            "Player::mGameMode storage is outside a readable memory region");
        return nullptr;
    }

    void* candidate = *reinterpret_cast<void* const*>(slot);
    if (!candidate) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "GameHooks", "game_mode_resolution", "player_field_null");
        return nullptr;
    }
    if (publicAttackVirtualEntry(candidate) == 0) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "GameHooks", "game_mode_resolution", "player_field_candidate_unverified",
            "candidate did not match a validated GameMode/SurvivalMode public attack vtable entry");
        return nullptr;
    }

    currentGameMode.store(candidate, std::memory_order_release);
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "GameHooks", "game_mode_resolution", "ready_from_player_field");
    return candidate;
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

    // GameMode::attack is the public three-argument wrapper; it prepares the
    // native context before invoking the internal body. Only dispatch through it
    // when the vtable entry matches a resolved public signature for this build.
    const auto publicEntry = publicAttackVirtualEntry(gameMode);
    if (publicEntry == 0) {
        static std::atomic_bool loggedUnavailable{false};
        if (!loggedUnavailable.exchange(true, std::memory_order_acq_rel)) {
            if (!plausibleGameMode(gameMode)) {
                bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error,
                    "GameHooks",
                    "attack_validation=implausible_gamemode: object/vtable did not pass the libminecraftpe validation");
            } else {
                auto** vtable = *reinterpret_cast<void***>(gameMode);
                constexpr std::size_t kKnownAttackSlot = 16;
                const auto vtableEntry = reinterpret_cast<std::uintptr_t>(vtable[kKnownAttackSlot]);
                const auto publicSignature = bedrocktools::memory::resolve(SignatureId::GameModeAttack);
                const auto survivalSignature = bedrocktools::memory::resolve(SignatureId::SurvivalModeAttack);
                char detail[320]{};
                std::snprintf(detail, sizeof(detail),
                    "attack_validation=vtable_mismatch: slot16=%p public_signature=%p survival_signature=%p",
                    reinterpret_cast<void*>(vtableEntry),
                    reinterpret_cast<void*>(publicSignature),
                    reinterpret_cast<void*>(survivalSignature));
                bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error,
                    "GameHooks", detail);
            }
            gameHooksLogger().error("Synthetic attack refused: public GameMode::attack is not validated for this runtime.");
        }
        return false;
    }
    auto** vtable = *reinterpret_cast<void***>(gameMode);
    constexpr std::size_t kKnownAttackSlot = 16;
    auto publicAttack = reinterpret_cast<PublicAttackFn>(vtable[kKnownAttackSlot]);
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
        gameHooksLogger().info("Combat dispatch selected the validated public GameMode::attack wrapper.");

    // The public wrapper calls the hooked internal body, where the standard
    // AttackEvent/GameModeAction/AttackResultEvent pipeline already runs.
    // Do not publish a second set here; observe and return the real internal result.
    publicAttack(gameMode, target, hitPosition);
    if (!canObserveNativeResult) {
        // The call went through a validated public engine entry, but this build
        // lacks the internal observer required to read the native bool result.
        // Return "dispatched" so callers can rate-limit/retry; this is not proof
        // that the target took damage.
        static std::atomic_bool loggedUnobservedDispatch{false};
        if (!loggedUnobservedDispatch.exchange(true, std::memory_order_acq_rel))
            gameHooksLogger().warn("Native attack used the validated public GameMode::attack entry, but the internal result hook is unavailable; damage confirmation is not observable.");
        return true;
    }
    if (!syntheticAttackResultObserved) {
        static std::atomic_bool loggedMissingInternalDispatch{false};
        if (!loggedMissingInternalDispatch.exchange(true, std::memory_order_acq_rel))
            gameHooksLogger().error("Validated public attack returned without reaching the installed internal observer; dispatch result is unknown.");
        return true;
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
