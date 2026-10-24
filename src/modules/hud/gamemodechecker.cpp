#include "gamemodechecker.hpp"

#include "../ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include "core/GameHooks.hpp"
#include "core/Diagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {
using ActorIsPlayerFn = bool (*)(void*);
using ActorGetNameTagFn = const std::string& (*)(const void*);
struct DistanceSortedActor {
    void* actor;
    float distance;
    float padding;
};
struct ActorVector {
    DistanceSortedActor* begin;
    DistanceSortedActor* end;
    DistanceSortedActor* capacity;
};
using ActorFetchNearbyActorsSortedFn = ActorVector (*)(void*, void*, int);

ActorFetchNearbyActorsSortedFn g_fetchNearbyActors = nullptr;
ActorIsPlayerFn g_actorIsPlayer = nullptr;
ActorGetNameTagFn g_actorGetNameTag = nullptr;
GamemodeCheckerModule* g_checker = nullptr;
std::atomic_bool stageActorClassifyBefore{false};
std::atomic_bool stageActorClassifyAfter{false};
std::atomic_bool stageModeBefore{false};
std::atomic_bool stageModeAfter{false};
std::atomic_bool stagePredicatesBefore{false};
std::atomic_bool stagePredicatesAfter{false};
std::atomic_bool stageNameBefore{false};
std::atomic_bool stageNameAfter{false};

void logStageOnce(std::atomic_bool& once, const char* message) {
    if (!once.exchange(true, std::memory_order_acq_rel)) {
        bedrocktools::diagnostics::log(
            bedrocktools::diagnostics::Level::Info, "GameModeChecker", message);
    }
}

bool trustedGameSymbol(void* address) {
    if (!address) return false;
    Dl_info info{};
    if (!dladdr(address, &info) || !info.dli_fname) return false;
    const std::string_view path(info.dli_fname);
    return path.find("libminecraftpe.so") != std::string_view::npos ||
        path.find("liblevilamina.so") != std::string_view::npos ||
        path.find("libLeviLamina.so") != std::string_view::npos;
}

void* resolveGameSymbol(const char* symbol) {
    if (!symbol) return nullptr;
    static void* minecraftHandle = nullptr;
    static void* leviLaminaHandle = nullptr;
    static void* leviLaminaAltHandle = nullptr;
    if (!minecraftHandle)
        minecraftHandle = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (!leviLaminaHandle)
        leviLaminaHandle = dlopen("liblevilamina.so", RTLD_NOW | RTLD_NOLOAD);
    if (!leviLaminaAltHandle)
        leviLaminaAltHandle = dlopen("libLeviLamina.so", RTLD_NOW | RTLD_NOLOAD);

    void* address = dlsym(RTLD_DEFAULT, symbol);
    if (trustedGameSymbol(address)) return address;
    if (minecraftHandle) {
        address = dlsym(minecraftHandle, symbol);
        if (trustedGameSymbol(address)) return address;
    }
    if (leviLaminaHandle) {
        address = dlsym(leviLaminaHandle, symbol);
        if (trustedGameSymbol(address)) return address;
    }
    if (leviLaminaAltHandle) {
        address = dlsym(leviLaminaAltHandle, symbol);
        if (trustedGameSymbol(address)) return address;
    }
    return nullptr;
}

std::string safeName(void* actor) {
    if (!bedrocktools::core::gamehooks::isValidGameObject(actor) || !g_actorGetNameTag ||
        !bedrocktools::core::gamehooks::isExecutableMemoryRange(
            reinterpret_cast<const void*>(g_actorGetNameTag))) return {};
    std::string name;
    // Use the build-resolved native getter only. Falling back to a guessed SDK
    // layout after a failed native call adds another unchecked fault path.
    logStageOnce(stageNameBefore, "stage=before_name_tag_getter");
    try { name = g_actorGetNameTag(actor); } catch (...) { name.clear(); }
    logStageOnce(stageNameAfter, "stage=after_name_tag_getter");
    if (name.size() > 96) name.resize(96);
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    }), name.end());
    const auto first = name.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = name.find_last_not_of(" \t\r\n");
    return name.substr(first, last - first + 1);
}
const char* modeName(std::int32_t mode) {
    switch (mode) {
        case 0: return "Survival";
        case 1: return "Creative";
        case 2: return "Adventure";
        case 3: return "Spectator";
        default: return "Unknown";
    }
}
}

GamemodeCheckerModule::GamemodeCheckerModule()
    : Module("Gamemode Checker", "Shows player modes and marks non-attackable players when the exact mode is unavailable.") {
    g_checker = this;
}

void GamemodeCheckerModule::onInit() {
    g_fetchNearbyActors = reinterpret_cast<ActorFetchNearbyActorsSortedFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));
    g_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));
    if (!g_actorGetNameTag) {
        g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
            resolveGameSymbol("_ZNK5Actor10getNameTagEv"));
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_checker && g_checker->enabled && event.player) {
                g_checker->refresh(event.player);
            }
        });
}

void GamemodeCheckerModule::onEnable() {
    m_ticks = 0;
}

void GamemodeCheckerModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_players.clear();
}

bool GamemodeCheckerModule::isSpectatorCandidate(void* actor) {
    // Aim Assist may consult this fallback. Only report Spectator when the
    // version-matched engine predicate is available; do not probe block
    // collisions/flight state as a speculative mode detector.
    std::int32_t mode = -1;
    return bedrocktools::core::gamehooks::tryGetPlayerCreativeSpectator(actor, mode) &&
        mode == 3;
}

void GamemodeCheckerModule::refresh(void* localPlayer) {
    // Keep native enumeration capped at 5 Hz on mobile.
    if (++m_ticks < 4) return;
    m_ticks = 0;

    // Runtime signatures can resolve after the module is initialized; retry missing
    // lookups on this throttled refresh instead of leaving the overlay permanently empty.
    if (!g_fetchNearbyActors) {
        g_fetchNearbyActors = reinterpret_cast<ActorFetchNearbyActorsSortedFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));
    }
    if (!g_actorIsPlayer) {
        g_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    }
    if (!g_actorGetNameTag) {
        g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));
    }
    if (!g_actorGetNameTag) {
        g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
            resolveGameSymbol("_ZNK5Actor10getNameTagEv"));
    }

    GamemodeCheckerDiagnostics diag{};
    diag.actorListReady = g_fetchNearbyActors != nullptr;
    diag.playerClassifierReady = g_actorIsPlayer != nullptr;
    diag.nameGetterReady = g_actorGetNameTag &&
        bedrocktools::core::gamehooks::isExecutableMemoryRange(
            reinterpret_cast<const void*>(g_actorGetNameTag));
    diag.localPlayerReady = localPlayer != nullptr;

    auto publishDiagnosticsOnly = [&]() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_diagnostics = diag;
        m_players.clear();
    };

    if (!localPlayer || !diag.actorListReady || !diag.playerClassifierReady ||
        !bedrocktools::core::gamehooks::isExecutableMemoryRange(
            reinterpret_cast<const void*>(g_fetchNearbyActors)) ||
        !bedrocktools::core::gamehooks::isExecutableMemoryRange(
            reinterpret_cast<const void*>(g_actorIsPlayer)) ||
        !bedrocktools::core::gamehooks::isValidGameObject(localPlayer)) {
        publishDiagnosticsOnly();
        return;
    }

    // Reuse the same build-matched nearby-actor query already used by Aim Assist
    // and Hitbox/ESP. This avoids dereferencing Level/ActorManager offsets here
    // and avoids returning a non-trivial std::vector across a guessed ABI.
    bedrocktools::sdk::Vec3 extent{80.0f, 80.0f, 80.0f};
    ActorVector actorVector{};
    static std::atomic_bool queryBeforeLogged{false};
    static std::atomic_bool queryAfterLogged{false};
    if (!queryBeforeLogged.exchange(true, std::memory_order_acq_rel)) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,
            "GameModeChecker", "stage=before_nearby_actor_query");
    }
    try {
        actorVector = g_fetchNearbyActors(localPlayer, &extent, 1);
    } catch (...) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning,
            "GameModeChecker", "stage=nearby_actor_query_exception");
        publishDiagnosticsOnly();
        return;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(actorVector.begin);
    const auto end = reinterpret_cast<std::uintptr_t>(actorVector.end);
    const auto capacity = reinterpret_cast<std::uintptr_t>(actorVector.capacity);
    const bool allNull = begin == 0 && end == 0 && capacity == 0;
    std::size_t actorCount = 0;
    if (!allNull) {
        if (begin <= 0x1000 || end < begin || capacity < end ||
            (end - begin) % sizeof(DistanceSortedActor) != 0 ||
            (capacity - begin) % sizeof(DistanceSortedActor) != 0 ||
            (end - begin) / sizeof(DistanceSortedActor) > 8192 ||
            (capacity - begin) / sizeof(DistanceSortedActor) > 16384 ||
            begin % alignof(DistanceSortedActor) != 0 ||
            ((end - begin) != 0 &&
             !bedrocktools::core::gamehooks::isReadableMemoryRange(
                 reinterpret_cast<const void*>(begin), end - begin))) {
            static std::atomic_bool badActorVectorLogged{false};
            if (!badActorVectorLogged.exchange(true, std::memory_order_acq_rel)) {
                bedrocktools::diagnostics::log(
                    bedrocktools::diagnostics::Level::Warning, "GameModeChecker",
                    "nearby_actor_query returned an invalid vector layout; refusing to read entries");
            }
            publishDiagnosticsOnly();
            return;
        }
        actorCount = (end - begin) / sizeof(DistanceSortedActor);
    }
    diag.actorCount = actorCount;
    diag.actorSnapshotReady = true;
    if (!queryAfterLogged.exchange(true, std::memory_order_acq_rel)) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,
            "GameModeChecker", "stage=after_nearby_actor_query entities=" +
            std::to_string(actorCount));
    }

    // The returned vector belongs to the live engine query. Validate that the
    // full used range remains in a readable mapping before copying entries; a
    // valid-looking begin/end/capacity triple alone is not sufficient.
    static std::atomic_bool directSnapshotLogged{false};
    if (!directSnapshotLogged.exchange(true, std::memory_order_acq_rel)) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,
            "GameModeChecker", "actor_snapshot_reader=direct_validated_native_vector");
    }
    std::vector<void*> actors;
    actors.reserve(actorCount);
    for (std::size_t i = 0; i < actorCount; ++i) {
        DistanceSortedActor entry{};
        std::memcpy(&entry, reinterpret_cast<const std::byte*>(actorVector.begin) +
                    i * sizeof(DistanceSortedActor), sizeof(entry));
        if (!std::isfinite(entry.distance) || entry.distance < 0.0f ||
            !bedrocktools::core::gamehooks::isValidGameObject(entry.actor) ||
            entry.actor == localPlayer) continue;
        actors.push_back(entry.actor);
    }

    std::vector<PlayerModeEntry> next;
    std::unordered_set<void*> seen;
    seen.reserve(actors.size());
    for (void* actor : actors) {
        if (!seen.emplace(actor).second) continue;

        bool isPlayer = false;
        logStageOnce(stageActorClassifyBefore, "stage=before_actor_classifier");
        if (!bedrocktools::core::gamehooks::isValidGameObject(actor) ||
            !bedrocktools::core::gamehooks::isExecutableMemoryRange(
                reinterpret_cast<const void*>(g_actorIsPlayer))) continue;
        try {
            isPlayer = g_actorIsPlayer(actor);
        } catch (...) {
            if (!stageActorClassifyAfter.exchange(true, std::memory_order_acq_rel)) {
                bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning,
                    "GameModeChecker", "stage=actor_classifier_exception");
            }
            continue;
        }
        if (!stageActorClassifyAfter.exchange(true, std::memory_order_acq_rel)) {
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,
                "GameModeChecker", std::string("stage=after_actor_classifier is_player=") +
                (isPlayer ? "1" : "0"));
        }
        if (!isPlayer) continue;
        ++diag.playerCount;

        std::int32_t gameType = -1;
        logStageOnce(stageModeBefore, "stage=before_player_mode_api");
        const bool hasGameType =
            bedrocktools::core::gamehooks::tryGetPlayerGameType(actor, gameType);
        if (!stageModeAfter.exchange(true, std::memory_order_acq_rel)) {
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,
                "GameModeChecker", std::string("stage=after_player_mode_api available=") +
                (hasGameType ? "1" : "0") + " raw=" + std::to_string(gameType));
        }
        const bool hasExactGameType = hasGameType && gameType >= 0 && gameType <= 3;
        std::int32_t predicateMode = -1;
        bool hasPredicateMode = false;
        if (!hasExactGameType) {
            logStageOnce(stagePredicatesBefore, "stage=before_creative_spectator_predicates");
            hasPredicateMode =
                bedrocktools::core::gamehooks::tryGetPlayerCreativeSpectator(actor, predicateMode);
            if (!stagePredicatesAfter.exchange(true, std::memory_order_acq_rel)) {
                bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,
                    "GameModeChecker", std::string("stage=after_creative_spectator_predicates available=") +
                    (hasPredicateMode ? "1" : "0") + " mode=" + std::to_string(predicateMode));
            }
        }

        std::string mode;
        bool inferred = false;
        if (hasExactGameType) {
            mode = modeName(gameType);
        } else if (hasPredicateMode && (predicateMode == 1 || predicateMode == 3)) {
            mode = std::string(modeName(predicateMode)) + "*";
            inferred = true;
        } else {
            // Do not guess from failed damage attempts or generic attackability:
            // those are combat heuristics, not a reliable per-player game mode.
            mode = "Unknown";
        }
        if (hasExactGameType) ++diag.exactModeCount;
        else if (inferred) ++diag.inferredModeCount;
        else ++diag.unresolvedModeCount;

        std::string name = safeName(actor);
        if (name.empty()) continue;
        ++diag.namedPlayerCount;
        next.push_back({std::move(name), std::move(mode)});
    }

    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
        return a.name < b.name;
    });
    std::lock_guard<std::mutex> lock(m_mutex);
    m_diagnostics = diag;
    m_players.swap(next);
}

void GamemodeCheckerModule::onFrame() {
    if (!enabled) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PLModMenu_DrawCommand> commands;
    constexpr float x = 18.0f;
    constexpr float y = 140.0f;
    constexpr float rowHeight = 24.0f;
    constexpr float width = 250.0f;
    const std::size_t count = std::min<std::size_t>(m_players.size(), 14);
    constexpr float statusHeight = 38.0f;

    PLModMenu_DrawCommand background{};
    background.type = PL_DRAW_RECT_FILLED;
    background.x = x;
    background.y = y;
    background.w = width;
    background.h = statusHeight + rowHeight * static_cast<float>(count + 1);
    background.color = 0xB0000000;
    commands.push_back(background);

    PLModMenu_DrawCommand title{};
    title.type = PL_DRAW_TEXT;
    title.x = x + 8.0f;
    title.y = y + 3.0f;
    title.w = 0.0f;
    title.h = 18.0f;
    title.size = 18.0f;
    title.color = 0xFFFFFFFF;
    title.text = "Gamemode Checker";
    commands.push_back(title);

    PLModMenu_DrawCommand status{};
    status.type = PL_DRAW_TEXT;
    status.x = x + 8.0f;
    status.y = y + 22.0f;
    status.w = 0.0f;
    status.h = 13.0f;
    status.size = 12.0f;
    status.color = 0xFFE0E0E0;
    const char* enumerator = !m_diagnostics.localPlayerReady ? "player:OFF" :
        (!m_diagnostics.actorListReady || !m_diagnostics.actorSnapshotReady)
            ? "query:OFF" : "query:OK";
    const char* modeApi = (m_diagnostics.exactModeCount > 0) ? "mode:EXACT" :
        (m_diagnostics.inferredModeCount > 0) ? "mode:INFERRED" :
        (m_diagnostics.playerCount > 0 ? "mode:UNKNOWN" : "mode:N/A");
    status.text = std::string(enumerator) + " [" +
        std::to_string(m_diagnostics.actorCount) + " entities, " +
        std::to_string(m_diagnostics.playerCount) + " players] " + modeApi;
    commands.push_back(status);

    for (std::size_t i = 0; i < count; ++i) {
        const auto& entry = m_players[i];
        PLModMenu_DrawCommand line{};
        line.type = PL_DRAW_TEXT;
        line.x = x + 8.0f;
        line.y = y + statusHeight + rowHeight * static_cast<float>(i + 1) + 3.0f;
        line.w = 0.0f;
        line.h = 16.0f;
        line.size = 16.0f;
        if (entry.mode == "Spectator" || entry.mode == "Spectator*") line.color = 0xFFB68CFF;
        else if (entry.mode == "Creative" || entry.mode == "Creative*") line.color = 0xFFFF4545;
        else if (entry.mode == "Creative/Spectator*") line.color = 0xFFFFC266;
        else if (entry.mode == "Unknown") line.color = 0xFFBBBBBB;
        else if (entry.mode == "Adventure") line.color = 0xFF8CC4FF;
        else line.color = 0xFFFFFFFF;
        line.text = entry.name + "  [" + entry.mode + "]";
        commands.push_back(line);
    }

    if (count == 0) {
        PLModMenu_DrawCommand empty{};
        empty.type = PL_DRAW_TEXT;
        empty.x = x + 8.0f;
        empty.y = y + statusHeight + rowHeight + 3.0f;
        empty.w = 0.0f;
        empty.h = 16.0f;
        empty.size = 16.0f;
        empty.color = 0xFFBBBBBB;
        empty.text = (m_diagnostics.playerCount > 0)
            ? "Players found, names unavailable"
            : (!m_diagnostics.actorListReady || !m_diagnostics.playerClassifierReady ||
               !m_diagnostics.localPlayerReady || !m_diagnostics.actorSnapshotReady)
                ? "Nearby-player query unavailable" : "No remote players found";
        commands.push_back(empty);
    }
    ::submitDrawCommands(moduleId, commands);
}