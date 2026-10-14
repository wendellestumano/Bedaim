#include "GameHooks.hpp"
#include <bedrocktools/Version.hpp>

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/AttackResultEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/offsets/UI.hpp>
#include <bedrocktools/sdk/offsets/World.hpp>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <cstdio>
#include <limits>
#include <pl/Logger.hpp>

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

pl::log::Logger& autoHitLogger() {
    static auto& logger = pl::log::Logger::getOrCreate("Bedaim-AutoHit");
    return logger;
}

// Check a pointer against the process's readable mappings before dereferencing
// memory obtained from a version-sensitive native object layout.
bool isReadableMemory(const void* pointer, std::size_t size) {
    if (!pointer || size == 0) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    if (size > std::numeric_limits<std::uintptr_t>::max() - begin) return false;
    const auto end = begin + size;

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char line[512]{};
    bool readable = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long regionBegin = 0;
        unsigned long long regionEnd = 0;
        char permissions[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &regionBegin, &regionEnd, permissions) == 3 &&
            permissions[0] == 'r' &&
            begin >= static_cast<std::uintptr_t>(regionBegin) &&
            end <= static_cast<std::uintptr_t>(regionEnd)) {
            readable = true;
            break;
        }
    }
    std::fclose(maps);
    return readable;
}

bool plausibleGameMode(void* gameMode) {
    if (!gameMode) return false;
    static std::atomic<void*> lastValidatedGameMode{nullptr};
    static std::atomic<void*> lastRejectedGameMode{nullptr};
    if (lastValidatedGameMode.load(std::memory_order_acquire) == gameMode) return true;
    // A bad field value can remain unchanged for thousands of ticks. Cache the
    // rejection as well, so validation never re-opens /proc/self/maps each tick.
    if (lastRejectedGameMode.load(std::memory_order_acquire) == gameMode) return false;

    constexpr std::size_t kKnownAttackSlot = 16;
    if (!isReadableMemory(gameMode, sizeof(void*))) {
        lastRejectedGameMode.store(gameMode, std::memory_order_release);
        return false;
    }
    auto** vtable = *reinterpret_cast<void***>(gameMode);
    if (!vtable || !isReadableMemory(vtable, (kKnownAttackSlot + 1) * sizeof(void*)) ||
        !vtable[kKnownAttackSlot]) {
        lastRejectedGameMode.store(gameMode, std::memory_order_release);
        return false;
    }

    Dl_info info{};
    if (!dladdr(vtable[kKnownAttackSlot], &info) || !info.dli_fname ||
        std::string_view(info.dli_fname).find("libminecraftpe.so") == std::string_view::npos) {
        lastRejectedGameMode.store(gameMode, std::memory_order_release);
        return false;
    }

    lastValidatedGameMode.store(gameMode, std::memory_order_release);
    lastRejectedGameMode.store(nullptr, std::memory_order_release);
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
    const AttackKind kind = attackKindFor(gameMode);
    const GameModeType modeKind = kind == AttackKind::SurvivalMode ? GameModeType::SurvivalMode : GameModeType::GameMode;
    GameModeActionScope scope{modeKind, gameMode};
    scope.publish(GameModeAction::Attack);
    AttackEvent event{kind, gameMode, reinterpret_cast<bedrocktools::sdk::Actor*>(target), reinterpret_cast<void*>(static_cast<std::uintptr_t>(playPredictiveSound)), const_cast<void*>(hitPosition)};
    bus().publish(event);
    if (event.cancelled()) return false;
    const bool result = gameModeAttackOriginal
        ? gameModeAttackOriginal(gameMode, target, playPredictiveSound, hitPosition)
        : false;
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
        // A cancellable FrameEvent can hold the last presented image for a couple
        // of frames on confirmed impacts, without pausing the game simulation.
        if (event.cancelled()) return EGL_TRUE;
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
    hookSignature(SignatureId::VersionString, reinterpret_cast<void*>(versionDetour), &versionOriginal);
    hookSignature(SignatureId::NormalTick, reinterpret_cast<void*>(tickDetour), &tickOriginal);
    hookSignature(SignatureId::GameModeStartDestroyBlock, reinterpret_cast<void*>(gameModeStartDestroyBlockDetour), &gameModeStartDestroyBlockOriginal);
    hookSignature(SignatureId::SurvivalModeStartDestroyBlock, reinterpret_cast<void*>(survivalModeStartDestroyBlockDetour), &survivalModeStartDestroyBlockOriginal);
    hookSignature(SignatureId::GameModeStopDestroyBlock, reinterpret_cast<void*>(gameModeStopDestroyBlockDetour), &gameModeStopDestroyBlockOriginal);
    hookSignature(SignatureId::GameModeStartBuildBlock, reinterpret_cast<void*>(gameModeStartBuildBlockDetour), &gameModeStartBuildBlockOriginal);
    hookSignature(SignatureId::SurvivalModeStartBuildBlock, reinterpret_cast<void*>(survivalModeStartBuildBlockDetour), &survivalModeStartBuildBlockOriginal);
    hookSignature(SignatureId::GameModeUseItem, reinterpret_cast<void*>(gameModeUseItemDetour), &gameModeUseItemOriginal);
    hookSignature(SignatureId::SurvivalModeUseItem, reinterpret_cast<void*>(survivalModeUseItemDetour), &survivalModeUseItemOriginal);
    hookSignature(SignatureId::GameModeUseItemAsAttack, reinterpret_cast<void*>(gameModeUseItemAsAttackDetour), &gameModeUseItemAsAttackOriginal);
    hookSignature(SignatureId::SurvivalModeUseItemAsAttack, reinterpret_cast<void*>(survivalModeUseItemAsAttackDetour), &survivalModeUseItemAsAttackOriginal);
    hookSignature(SignatureId::GameModeUseItemOn, reinterpret_cast<void*>(gameModeUseItemOnDetour), &gameModeUseItemOnOriginal);
    hookSignature(SignatureId::SurvivalModeUseItemOn, reinterpret_cast<void*>(survivalModeUseItemOnDetour), &survivalModeUseItemOnOriginal);
    hookSignature(SignatureId::GameModeInteract, reinterpret_cast<void*>(gameModeInteractDetour), &gameModeInteractOriginal);
    hookSignature(SignatureId::SurvivalModeInteract, reinterpret_cast<void*>(survivalModeInteractDetour), &survivalModeInteractOriginal);
    const bool internalAttackHooked = hookSignature(
        SignatureId::GameModeAttackInternal, reinterpret_cast<void*>(gameModeAttackDetour), &gameModeAttackOriginal);
    autoHitLogger().info(
        "Attack setup: public signature={}, survival signature={}, internal signature={}, internal hook={}, trampoline={}",
        bedrocktools::memory::resolve(SignatureId::GameModeAttack) != 0,
        bedrocktools::memory::resolve(SignatureId::SurvivalModeAttack) != 0,
        bedrocktools::memory::resolve(SignatureId::GameModeAttackInternal) != 0,
        internalAttackHooked,
        gameModeAttackOriginal != nullptr);
    hookSignature(SignatureId::ClientInstanceUpdate, reinterpret_cast<void*>(clientUpdateDetour), &clientUpdateOriginal);
    hookSignature(SignatureId::ContainerScreenControllerOpen, reinterpret_cast<void*>(containerOpenDetour), &containerOpenOriginal);
    hookSignature(SignatureId::ContainerScreenControllerDtor, reinterpret_cast<void*>(containerCloseDetour), &containerCloseOriginal);
    hookSignature(SignatureId::ChatScreenOpen, reinterpret_cast<void*>(chatOpenDetour), &chatOpenOriginal);
    hookSignature(SignatureId::ChatScreenDtor, reinterpret_cast<void*>(chatCloseDetour), &chatCloseOriginal);
    hookSignature(SignatureId::ContainerScreenControllerOnContainerSlotSelected, reinterpret_cast<void*>(containerSlotSelectedDetour), &containerSlotSelectedOriginal);
    hookEgl();
    installed = tickOriginal != nullptr && clientUpdateOriginal != nullptr;
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
}

void* clientInstance() {
    return currentClientInstance.load(std::memory_order_acquire);
}

void* gameMode() {
    return currentGameMode.load(std::memory_order_acquire);
}

void* gameModeForPlayer(void* player) {
    if (!player) return nullptr;

    // Cache only the player field address after confirming it is mapped readable.
    // The value can still change (e.g. after respawn), so re-read the pointer each tick.
    static std::atomic<void*> cachedPlayer{nullptr};
    static std::atomic<void*> cachedField{nullptr};
    const auto playerAddress = reinterpret_cast<std::uintptr_t>(player);
    const auto fieldAddress = reinterpret_cast<const void*>(
        playerAddress + bedrocktools::sdk::offsets::Player::mGameMode);
    void* candidate = nullptr;
    if (cachedPlayer.load(std::memory_order_acquire) == player &&
        cachedField.load(std::memory_order_acquire) == fieldAddress) {
        candidate = *reinterpret_cast<void* const*>(fieldAddress);
    } else if (isReadableMemory(fieldAddress, sizeof(void*))) {
        candidate = *reinterpret_cast<void* const*>(fieldAddress);
        cachedField.store(const_cast<void*>(fieldAddress), std::memory_order_release);
        cachedPlayer.store(player, std::memory_order_release);
    } else {
        cachedField.store(nullptr, std::memory_order_release);
        cachedPlayer.store(player, std::memory_order_release);
    }

    if (plausibleGameMode(candidate)) {
        currentGameMode.store(candidate, std::memory_order_release);
        return candidate;
    }

    // A GameMode captured from a real engine callback is preferred over an
    // unverified layout guess. Validate it as well before allowing a native call.
    void* observed = currentGameMode.load(std::memory_order_acquire);
    return plausibleGameMode(observed) ? observed : nullptr;
}

bool attack(void* gameMode, void* target, bool /*playPredictiveSound*/, const void* hitPosition) {
    if (!gameMode || !target || !hitPosition) return false;

    static thread_local bool inSyntheticAttack = false;
    if (inSyntheticAttack) return false;
    if (currentGameMode.load(std::memory_order_acquire) != gameMode) return false;

    AttackKind kind = AttackKind::GameMode;
    const auto virtualEntry = publicAttackVirtualEntry(gameMode, &kind);
    if (!virtualEntry) {
        static std::atomic_bool loggedUnavailable{false};
        if (!loggedUnavailable.exchange(true, std::memory_order_acq_rel))
            autoHitLogger().error("Attack skipped safely: vtable slot 16 did not match this build's public GameMode::attack signature. No native attack body was called.");
        return false;
    }

    struct AttackGuard {
        bool& active;
        explicit AttackGuard(bool& value) : active(value) { active = true; }
        ~AttackGuard() { active = false; }
    } guard{inSyntheticAttack};

    static std::atomic_bool loggedPublicRoute{false};
    if (!loggedPublicRoute.exchange(true, std::memory_order_acq_rel))
        autoHitLogger().info("Combat dispatch uses the signature-validated public GameMode::attack wrapper.");

    // This public wrapper establishes Bedrock's internal attack flags and then
    // routes through the hooked native implementation. That is the same entry
    // point vanilla uses; do not manually replay the internal 4-argument ABI.
    using PublicAttackFn = bool(*)(void*, void*, const void*);
    return reinterpret_cast<PublicAttackFn>(virtualEntry)(gameMode, target, hitPosition);
}

}
