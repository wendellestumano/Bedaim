#include "playerradar.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include "core/GameHooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>

namespace {
struct RadarActorEntry {
    void* actor;
    float distance;
    float padding;
};
struct RadarActorVector {
    RadarActorEntry* begin;
    RadarActorEntry* end;
    RadarActorEntry* capacity;
};
using FetchNearbyActorsFn = RadarActorVector (*)(void*, void*, int);
using ActorIsPlayerFn = bool (*)(void*);
using ActorGetNameTagFn = std::string (*)(void*);

FetchNearbyActorsFn g_fetchNearbyActors = nullptr;
ActorIsPlayerFn g_actorIsPlayer = nullptr;
ActorGetNameTagFn g_actorGetNameTag = nullptr;

std::string cleanName(std::string name) {
    if (name.size() > 64) name.resize(64);
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    }), name.end());
    return name;
}
}

PlayerRadarModule::PlayerRadarModule()
    : Module("Player Radar", "Shows nearby player names and their distance in blocks.") {
    keybind = 0;
}

void PlayerRadarModule::onInit() {
    g_fetchNearbyActors = reinterpret_cast<FetchNearbyActorsFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));
    g_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) {
            if (!enabled || !event.player || !g_fetchNearbyActors || !g_actorIsPlayer) return;
            float range = 80.0f;
            int maxPlayers = 8;
            {
                std::lock_guard lock(m_mutex);
                range = std::clamp(m_range, 5.0f, 80.0f);
                maxPlayers = std::clamp(m_maxPlayers, 1, 32);
            }
            auto* local = reinterpret_cast<bedrocktools::sdk::Actor*>(event.player);
            const auto origin = local->position();
            if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z)) return;
            bedrocktools::sdk::Vec3 extent{range, range, range};
            const auto actors = g_fetchNearbyActors(event.player, &extent, 1);
            if (!actors.begin || !actors.end) return;
            const auto begin = reinterpret_cast<std::uintptr_t>(actors.begin);
            const auto end = reinterpret_cast<std::uintptr_t>(actors.end);
            if (end < begin || (end - begin) % sizeof(RadarActorEntry) != 0 ||
                (end - begin) / sizeof(RadarActorEntry) > 4096) return;

            std::vector<Entry> found;
            found.reserve(static_cast<std::size_t>(maxPlayers));
            for (auto* it = actors.begin; it < actors.end; ++it) {
                void* actor = it->actor;
                if (!actor || actor == event.player || !g_actorIsPlayer(actor)) continue;
                auto* target = reinterpret_cast<bedrocktools::sdk::Actor*>(actor);
                const auto pos = target->position();
                if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) continue;
                const float dx = pos.x - origin.x;
                const float dy = pos.y - origin.y;
                const float dz = pos.z - origin.z;
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (!std::isfinite(distance) || distance > range) continue;
                // Use the in-repository Player SDK accessor instead of a guessed
                // C++ symbol/ABI. ActorGetNameTag remains a fallback for empty names.
                const auto* playerActor = reinterpret_cast<const bedrocktools::sdk::Player*>(actor);
                std::string name = cleanName(playerActor->name());
                if (name.empty() && g_actorGetNameTag) name = cleanName(g_actorGetNameTag(actor));
                if (name.empty()) name = "Player";
                std::int32_t gameType = -1;
                std::string mode = "Unknown";
                if (bedrocktools::core::gamehooks::tryGetPlayerGameType(actor, gameType)) {
                    switch (gameType) {
                        case 0: mode = "Survival"; break;
                        case 1: mode = "Creative"; break;
                        case 2: mode = "Adventure"; break;
                        case 3: mode = "Spectator"; break;
                        default: break;
                    }
                }
                found.push_back({std::move(name), distance, std::move(mode)});
            }
            std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) {
                return a.distance < b.distance;
            });
            if (found.size() > static_cast<std::size_t>(maxPlayers))
                found.resize(static_cast<std::size_t>(maxPlayers));
            std::lock_guard lock(m_mutex);
            m_players.swap(found);
        });
}

void PlayerRadarModule::onEnable() {
    std::lock_guard lock(m_mutex);
    m_players.clear();
}

void PlayerRadarModule::onDisable() {
    std::lock_guard lock(m_mutex);
    m_players.clear();
    pl::modmenu::submitDrawCommands(moduleId, {});
}

void PlayerRadarModule::onFrame() {
    if (!enabled) return;
    std::vector<Entry> players;
    float x, y, size, opacity;
    bool background;
    {
        std::lock_guard lock(m_mutex);
        players = m_players;
        x = hudPosX;
        y = hudPosY;
        size = std::clamp(m_textSize, 8.0f, 32.0f);
        opacity = std::clamp(m_backgroundOpacity, 0.0f, 1.0f);
        background = m_background;
    }
    std::vector<pl::modmenu::DrawCommand> commands;
    const float rowHeight = size + 5.0f;
    const float width = 300.0f;
    const float height = rowHeight * static_cast<float>(std::max<std::size_t>(players.size(), 1)) + 8.0f;
    if (background) {
        pl::modmenu::DrawCommand bg{};
        bg.type = pl::modmenu::DrawCommandType::RectFilled;
        bg.x = x; bg.y = y; bg.w = width; bg.h = height;
        bg.color = static_cast<std::uint32_t>(opacity * 255.0f) << 24U;
        commands.push_back(std::move(bg));
    }
    if (players.empty()) {
        pl::modmenu::DrawCommand label{};
        label.type = pl::modmenu::DrawCommandType::Text;
        label.x = x + 5.0f; label.y = y + 4.0f;
        label.w = width - 10.0f; label.h = rowHeight;
        label.size = size; label.color = 0xFFFFFFFF;
        label.text = "Player Radar: no players nearby";
        commands.push_back(std::move(label));
    } else {
        for (std::size_t i = 0; i < players.size(); ++i) {
            char distance[32]{};
            std::snprintf(distance, sizeof(distance), "%.1f blocks", players[i].distance);
            pl::modmenu::DrawCommand label{};
            label.type = pl::modmenu::DrawCommandType::Text;
            label.x = x + 5.0f;
            label.y = y + 4.0f + rowHeight * static_cast<float>(i);
            label.w = width - 10.0f; label.h = rowHeight;
            label.size = size; label.color = 0xFFFFFFFF;
            label.text = players[i].name + "  -  " + distance + "  [" + players[i].gameMode + "]";
            commands.push_back(std::move(label));
        }
    }
    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void PlayerRadarModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("radar")
        .category("radar", "Radar", "Nearby player detection")
        .category("appearance", "Appearance", "HUD position and style");
    auto slider = [&](const char* key, const char* title, const char* category,
                      const char* minimum, const char* maximum, const char* step, const char* unit) {
        ConfigNodeV2 node;
        node.id = key; node.key = key; node.title = title; node.category = category;
        node.type = ConfigControlTypeV2::SliderFloat;
        node.minValue = minimum; node.maxValue = maximum; node.step = step; node.unit = unit;
        schema.node(std::move(node));
    };
    slider("m_range", "Detection range", "radar", "5", "80", "1", " blocks");
    slider("m_maxPlayers", "Maximum listed players", "radar", "1", "32", "1", " players");
    slider("m_textSize", "Text size", "appearance", "8", "32", "1", " px");
    slider("m_backgroundOpacity", "Background opacity", "appearance", "0", "1", "0.05", "");
    ConfigNodeV2 bg;
    bg.id = "m_background"; bg.key = "m_background"; bg.title = "Background";
    bg.category = "appearance"; bg.type = ConfigControlTypeV2::Toggle;
    schema.node(std::move(bg));
    ConfigNodeV2 keybind;
    keybind.id = "keybind"; keybind.key = "keybind"; keybind.title = "Keybind";
    keybind.category = "radar"; keybind.type = ConfigControlTypeV2::Keybind;
    schema.node(std::move(keybind));
    setConfigSchemaJson(moduleId, schema.toJson());
}

void PlayerRadarModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard lock(m_mutex);
    if (json.contains("hudPosX") && json["hudPosX"].is_number()) hudPosX = std::clamp(json["hudPosX"].get<float>(), 0.0f, 4000.0f);
    if (json.contains("hudPosY") && json["hudPosY"].is_number()) hudPosY = std::clamp(json["hudPosY"].get<float>(), 0.0f, 4000.0f);
    if (json.contains("m_range") && json["m_range"].is_number()) m_range = std::clamp(json["m_range"].get<float>(), 5.0f, 80.0f);
    if (json.contains("m_maxPlayers") && json["m_maxPlayers"].is_number()) m_maxPlayers = std::clamp(json["m_maxPlayers"].get<int>(), 1, 32);
    if (json.contains("m_textSize") && json["m_textSize"].is_number()) m_textSize = std::clamp(json["m_textSize"].get<float>(), 8.0f, 32.0f);
    if (json.contains("m_backgroundOpacity") && json["m_backgroundOpacity"].is_number()) m_backgroundOpacity = std::clamp(json["m_backgroundOpacity"].get<float>(), 0.0f, 1.0f);
    if (json.contains("m_background") && json["m_background"].is_boolean()) m_background = json["m_background"].get<bool>();
}

void PlayerRadarModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard lock(m_mutex);
    json["hudPosX"] = hudPosX; json["hudPosY"] = hudPosY; json["isHudModule"] = true;
    json["m_range"] = m_range; json["m_maxPlayers"] = m_maxPlayers;
    json["m_textSize"] = m_textSize; json["m_backgroundOpacity"] = m_backgroundOpacity;
    json["m_background"] = m_background;
}
