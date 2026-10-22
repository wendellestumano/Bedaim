#include "gamemodechecker.hpp"

#include "../ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include "core/GameHooks.hpp"

#include <algorithm>
#include <cstdint>
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
    : Module("Gamemode Checker", "Lists nearby players confirmed as Creative or Spectator.") {
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
    // Limit native actor-list and GameType lookups to 5 Hz on mobile.
    if (++m_ticks < 4) return;
    m_ticks = 0;
    if (!localPlayer || !g_getRuntimeActorList || !g_actorIsPlayer) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_players.clear();
        return;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(localPlayer);
    void* level = *reinterpret_cast<void**>(
        base + bedrocktools::sdk::offsets::Actor::mLevel);
    if (!level) return;
    void* actorManager = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(level) +
        bedrocktools::sdk::offsets::Level::mActorManager);
    if (!actorManager) return;

    std::vector<void*> actors;
    try {
        actors = g_getRuntimeActorList(actorManager);
    } catch (...) {
        return;
    }
    if (actors.size() > 4096) return;

    std::vector<PlayerModeEntry> next;
    std::unordered_set<void*> seen;
    seen.reserve(actors.size());
    for (void* actor : actors) {
        if (!actor || actor == localPlayer || !seen.emplace(actor).second) continue;
        if (!g_actorIsPlayer(actor) || !g_actorGetNameTag) continue;

        std::int32_t gameType = -1;
        if (!bedrocktools::core::gamehooks::tryGetPlayerGameType(actor, gameType)) continue;
        if (gameType != 1 && gameType != 3) continue;

        std::string name = safeName(actor);
        if (name.empty()) continue;
        next.push_back({std::move(name), modeName(gameType)});
    }
    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
        return a.name < b.name;
    });
    std::lock_guard<std::mutex> lock(m_mutex);
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
    const std::size_t count = std::min<std::size_t>(m_players.size(), 16);

    PLModMenu_DrawCommand background{};
    background.type = PL_DRAW_RECT_FILLED;
    background.x = x;
    background.y = y;
    background.w = width;
    background.h = rowHeight * static_cast<float>(count + 1);
    background.color = 0xB0000000;
    commands.push_back(background);

    PLModMenu_DrawCommand title{};
    title.type = PL_DRAW_TEXT;
    title.x = x + 8.0f;
    title.y = y + 5.0f;
    title.w = 0.0f;
    title.h = 18.0f;
    title.size = 18.0f;
    title.color = 0xFFFFFFFF;
    title.text = "Player Gamemodes";
    commands.push_back(title);

    for (std::size_t i = 0; i < count; ++i) {
        const auto& entry = m_players[i];
        PLModMenu_DrawCommand line{};
        line.type = PL_DRAW_TEXT;
        line.x = x + 8.0f;
        line.y = y + rowHeight * static_cast<float>(i + 1) + 3.0f;
        line.w = 0.0f;
        line.h = 16.0f;
        line.size = 16.0f;
        line.color = entry.mode == "Spectator" ? 0xFFB68CFF : 0xFF65E6A3;
        line.text = entry.name + "  [" + entry.mode + "]";
        commands.push_back(line);
    }

    if (count == 0) {
        PLModMenu_DrawCommand empty{};
        empty.type = PL_DRAW_TEXT;
        empty.x = x + 8.0f;
        empty.y = y + rowHeight + 3.0f;
        empty.w = 0.0f;
        empty.h = 16.0f;
        empty.size = 16.0f;
        empty.color = 0xFFBBBBBB;
        empty.text = "No confirmed Creative/Spectator";
        commands.push_back(empty);
    }
    ::submitDrawCommands(moduleId, commands);
}
