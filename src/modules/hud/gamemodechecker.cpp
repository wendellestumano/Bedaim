#include "gamemodechecker.hpp"

#include "../ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include "core/GameHooks.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
using GetRuntimeActorListFn = std::vector<void*> (*)(void*);
using ActorIsPlayerFn = bool (*)(void*);
using ActorGetNameTagFn = std::string (*)(void*);

GetRuntimeActorListFn g_getRuntimeActorList = nullptr;
ActorIsPlayerFn g_actorIsPlayer = nullptr;
ActorGetNameTagFn g_actorGetNameTag = nullptr;
GamemodeCheckerModule* g_checker = nullptr;

std::string safeName(void* actor) {
    if (!actor || !g_actorGetNameTag) return {};
    try {
        std::string name = g_actorGetNameTag(actor);
        if (name.size() > 96) name.resize(96);
        name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) {
            return c < 0x20 || c == 0x7f;
        }), name.end());
        return name;
    } catch (...) {
        return {};
    }
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
    g_getRuntimeActorList = reinterpret_cast<GetRuntimeActorListFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList));
    g_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));

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

void GamemodeCheckerModule::refresh(void* localPlayer) {
    // Keep native enumeration capped at 5 Hz on mobile.
    if (++m_ticks < 4) return;
    m_ticks = 0;

    GamemodeCheckerDiagnostics diag{};
    diag.actorListReady = g_getRuntimeActorList != nullptr;
    diag.playerClassifierReady = g_actorIsPlayer != nullptr;
    diag.nameGetterReady = g_actorGetNameTag != nullptr;

    auto publishDiagnosticsOnly = [&]() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_diagnostics = diag;
        m_players.clear();
    };

    if (!localPlayer || !diag.actorListReady || !diag.playerClassifierReady) {
        publishDiagnosticsOnly();
        return;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(localPlayer);
    if (base > std::numeric_limits<std::uintptr_t>::max() -
                   bedrocktools::sdk::offsets::Actor::mLevel - sizeof(void*)) {
        publishDiagnosticsOnly();
        return;
    }

    void* level = nullptr;
    std::memcpy(&level, reinterpret_cast<const void*>(
        base + bedrocktools::sdk::offsets::Actor::mLevel), sizeof(level));
    diag.levelReady = level != nullptr;
    if (!level) {
        publishDiagnosticsOnly();
        return;
    }

    const auto levelAddress = reinterpret_cast<std::uintptr_t>(level);
    if (levelAddress > std::numeric_limits<std::uintptr_t>::max() -
                           bedrocktools::sdk::offsets::Level::mActorManager - sizeof(void*)) {
        publishDiagnosticsOnly();
        return;
    }
    void* actorManager = nullptr;
    std::memcpy(&actorManager, reinterpret_cast<const void*>(
        levelAddress + bedrocktools::sdk::offsets::Level::mActorManager), sizeof(actorManager));
    diag.actorManagerReady = actorManager != nullptr;
    if (!actorManager) {
        publishDiagnosticsOnly();
        return;
    }

    std::vector<void*> actors;
    try {
        actors = g_getRuntimeActorList(actorManager);
    } catch (...) {
        publishDiagnosticsOnly();
        return;
    }
    if (actors.size() > 4096) {
        diag.actorCount = actors.size();
        publishDiagnosticsOnly();
        return;
    }
    diag.actorCount = actors.size();

    std::vector<PlayerModeEntry> next;
    std::unordered_set<void*> seen;
    seen.reserve(actors.size());
    for (void* actor : actors) {
        if (!actor || actor == localPlayer || !seen.emplace(actor).second) continue;

        bool isPlayer = false;
        try {
            isPlayer = g_actorIsPlayer(actor);
        } catch (...) {
            continue;
        }
        if (!isPlayer) continue;
        ++diag.playerCount;
        if (!g_actorGetNameTag) continue;

        std::int32_t gameType = -1;
        const bool hasExactGameType =
            bedrocktools::core::gamehooks::tryGetPlayerGameType(actor, gameType);
        std::int32_t predicateMode = -1;
        const bool hasPredicateState =
            bedrocktools::core::gamehooks::tryGetPlayerGameType(actor, predicateMode) &&
            (predicateMode == 1 || predicateMode == 3);
        bool attackable = true;
        const bool hasAttackability = !hasPredicateState &&
            bedrocktools::core::gamehooks::tryIsActorAttackableGamemode(actor, attackable);

        std::string mode;
        if (hasPredicateState) {
            mode = modeName(predicateMode);
        } else if (hasExactGameType) {
            mode = modeName(gameType);
        } else if (hasAttackability && !attackable) {
            mode = "Creative/Spectator*";
        } else {
            mode = "Unknown";
        }

        if (mode != "Unknown" && mode != "Creative/Spectator*") ++diag.exactModeCount;
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
    const char* enumerator = !m_diagnostics.actorListReady ? "actors:OFF" :
        !m_diagnostics.levelReady ? "level:OFF" :
        !m_diagnostics.actorManagerReady ? "manager:OFF" : "actors:OK";
    const char* modeApi = (m_diagnostics.exactModeCount > 0) ? "mode:OK" :
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
        if (entry.mode == "Spectator") line.color = 0xFFB68CFF;
        else if (entry.mode == "Creative") line.color = 0xFF65E6A3;
        else if (entry.mode == "Creative/Spectator*") line.color = 0xFFFFC266;
        else if (entry.mode == "Unknown") line.color = 0xFFBBBBBB;
        else if (entry.mode == "Other (not C/S)") line.color = 0xFF8CC4FF;
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
               !m_diagnostics.levelReady || !m_diagnostics.actorManagerReady)
                ? "Enumeration path unavailable" : "No remote players found";
        commands.push_back(empty);
    }
    ::submitDrawCommands(moduleId, commands);
}
