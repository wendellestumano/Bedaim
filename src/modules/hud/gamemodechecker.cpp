#include "gamemodechecker.hpp"

#include "../ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include "core/GameHooks.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
struct DistanceSortedActor {
    void* mActor;
    float mDistance;
    float _pad;
};
struct ActorVec {
    DistanceSortedActor* begin;
    DistanceSortedActor* end;
    DistanceSortedActor* cap;
};
using ActorFetchNearbyActorsSortedFn = ActorVec (*)(void*, void*, int);
using ActorIsPlayerFn = bool (*)(void*);
using ActorIsInvisibleFn = bool (*)(void*);
using ActorGetNameTagFn = const std::string& (*)(const void*);
using BlockSourceIsSolidBlockingBlockFn =
    bool (*)(void*, const bedrocktools::sdk::BlockPos*);

ActorFetchNearbyActorsSortedFn g_actorFetchNearbyActorsSorted = nullptr;
ActorIsPlayerFn g_actorIsPlayer = nullptr;
ActorIsInvisibleFn g_actorIsInvisible = nullptr;
ActorGetNameTagFn g_actorGetNameTag = nullptr;
BlockSourceIsSolidBlockingBlockFn g_isSolidBlockingBlock = nullptr;
GamemodeCheckerModule* g_checker = nullptr;

struct SpectatorProbe {
    bedrocktools::sdk::Vec3 lastPosition{};
    unsigned consecutiveNoclipSamples = 0;
    bool hasLastPosition = false;
    std::chrono::steady_clock::time_point lastSeen{};
};
std::unordered_map<void*, SpectatorProbe> g_spectatorProbes;
std::mutex g_spectatorProbeMutex;

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

bool hasSolidBlockInsideBody(void* actor) {
    if (!actor || !g_isSolidBlockingBlock) return false;
    const auto* nativeActor = reinterpret_cast<const bedrocktools::sdk::Actor*>(actor);
    const auto bounds = nativeActor->bounds();
    const float height = bounds.max.y - bounds.min.y;
    if (!std::isfinite(bounds.min.x) || !std::isfinite(bounds.min.y) ||
        !std::isfinite(bounds.min.z) || !std::isfinite(bounds.max.x) ||
        !std::isfinite(bounds.max.y) || !std::isfinite(bounds.max.z)) return false;

    auto* dimension = nativeActor->dimension();
    void* blockSource = dimension ? static_cast<void*>(dimension->blockSource()) : nullptr;
    if (!blockSource) return false;

    // A spectator-like actor can have no usable body AABB. In that case sample
    // the actor root inside a solid voxel instead of silently dropping the test.
    if (height < 1.0f || height > 4.0f ||
        bounds.max.x - bounds.min.x < 0.1f || bounds.max.z - bounds.min.z < 0.1f) {
        const auto position = nativeActor->position();
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z)) return false;
        const bedrocktools::sdk::BlockPos pos{
            static_cast<int>(std::floor(position.x)),
            static_cast<int>(std::floor(position.y + 0.7f)),
            static_cast<int>(std::floor(position.z))
        };
        return g_isSolidBlockingBlock(blockSource, &pos);
    }

    const float x = (bounds.min.x + bounds.max.x) * 0.5f;
    const float z = (bounds.min.z + bounds.max.z) * 0.5f;
    const float samples[] = {
        bounds.min.y + 0.25f,
        (bounds.min.y + bounds.max.y) * 0.5f,
        bounds.max.y - 0.25f
    };
    for (const float y : samples) {
        if (y <= bounds.min.y || y >= bounds.max.y) continue;
        const bedrocktools::sdk::BlockPos pos{
            static_cast<int>(std::floor(x)),
            static_cast<int>(std::floor(y)),
            static_cast<int>(std::floor(z))
        };
        if (g_isSolidBlockingBlock(blockSource, &pos)) return true;
    }
    return false;
}
bool hasSpectatorNoclipEvidence(void* actor) {
    // Player::isFlying and Actor::getStatusFlag are commonly stripped from the
    // runtime's dynamic symbol table. Use Bedaim's already-resolved ActorIsInvisible
    // signature plus actual solid-block overlap, which is much stronger evidence
    // than invisibility alone and works without exported method names.
    if (!actor || !g_actorIsInvisible || !g_isSolidBlockingBlock) return false;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> probeLock(g_spectatorProbeMutex);
    auto& probe = g_spectatorProbes[actor];

    if (probe.hasLastPosition && now - probe.lastSeen < std::chrono::milliseconds(150))
        return probe.consecutiveNoclipSamples >= 2;

    bool invisible = false;
    try {
        invisible = g_actorIsInvisible(actor);
    } catch (...) {
        probe.lastSeen = now;
        return false;
    }

    const auto* nativeActor = reinterpret_cast<const bedrocktools::sdk::Actor*>(actor);
    const auto position = nativeActor->position();
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) {
        probe.lastSeen = now;
        return false;
    }
    const bool moving = probe.hasLastPosition &&
        (std::abs(position.x - probe.lastPosition.x) > 0.12f ||
         std::abs(position.y - probe.lastPosition.y) > 0.12f ||
         std::abs(position.z - probe.lastPosition.z) > 0.12f);
    probe.lastPosition = position;
    probe.hasLastPosition = true;
    probe.lastSeen = now;

    if (!invisible || !moving) {
        probe.consecutiveNoclipSamples = 0;
        return false;
    }

    if (hasSolidBlockInsideBody(actor))
        probe.consecutiveNoclipSamples = std::min(probe.consecutiveNoclipSamples + 1u, 5u);
    else
        probe.consecutiveNoclipSamples = 0;
    return probe.consecutiveNoclipSamples >= 2;
}
std::string safeName(void* actor) {
    if (!actor) return {};
    std::string name;
    // Prefer the exported native Actor::getNameTag API; keep the SDK field as fallback.
    if (g_actorGetNameTag) {
        try { name = g_actorGetNameTag(actor); } catch (...) { name.clear(); }
    }
    if (name.empty()) {
        try { name = reinterpret_cast<const bedrocktools::sdk::Player*>(actor)->name(); }
        catch (...) { name.clear(); }
    }
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
    g_actorFetchNearbyActorsSorted = reinterpret_cast<ActorFetchNearbyActorsSortedFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));
    g_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));
    if (!g_actorGetNameTag) {
        g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
            resolveGameSymbol("_ZNK5Actor10getNameTagEv"));
    }
    g_actorIsInvisible = reinterpret_cast<ActorIsInvisibleFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsInvisible));
    g_isSolidBlockingBlock = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock));

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
    return hasSpectatorNoclipEvidence(actor);
}

void GamemodeCheckerModule::refresh(void* localPlayer) {
    // Nearby actor query is shared with Hitbox/ESP and avoids the previous
    // Level/ActorManager mix-up that made the checker report actors:OFF.
    if (++m_ticks < 4) return;
    m_ticks = 0;

    if (!g_actorFetchNearbyActorsSorted) {
        g_actorFetchNearbyActorsSorted = reinterpret_cast<ActorFetchNearbyActorsSortedFn>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));
    }
    if (!g_actorIsPlayer) {
        g_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    }
    if (!g_actorIsInvisible) {
        g_actorIsInvisible = reinterpret_cast<ActorIsInvisibleFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsInvisible));
    }
    if (!g_actorGetNameTag) {
        g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));
    }
    if (!g_actorGetNameTag) {
        g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
            resolveGameSymbol("_ZNK5Actor10getNameTagEv"));
    }
    if (!g_isSolidBlockingBlock) {
        g_isSolidBlockingBlock = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock));
    }

    const auto probeNow = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> probeLock(g_spectatorProbeMutex);
        for (auto it = g_spectatorProbes.begin(); it != g_spectatorProbes.end();) {
            if (probeNow - it->second.lastSeen > std::chrono::seconds(12))
                it = g_spectatorProbes.erase(it);
            else
                ++it;
        }
    }

    GamemodeCheckerDiagnostics diag{};
    diag.actorQueryReady = g_actorFetchNearbyActorsSorted != nullptr;
    diag.playerClassifierReady = g_actorIsPlayer != nullptr;
    diag.nameGetterReady = g_actorGetNameTag != nullptr;

    auto publishDiagnosticsOnly = [&]() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_diagnostics = diag;
        m_players.clear();
    };

    if (!localPlayer || !diag.actorQueryReady || !diag.playerClassifierReady) {
        publishDiagnosticsOnly();
        return;
    }

    bedrocktools::sdk::Vec3 extent{80.0f, 80.0f, 80.0f};
    ActorVec actors{};
    try {
        // Type 1 is the same query category used by the working Hitbox module.
        actors = g_actorFetchNearbyActorsSorted(localPlayer, &extent, 1);
    } catch (...) {
        publishDiagnosticsOnly();
        return;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(actors.begin);
    const auto end = reinterpret_cast<std::uintptr_t>(actors.end);
    const auto cap = reinterpret_cast<std::uintptr_t>(actors.cap);
    if (!actors.begin && !actors.end && !actors.cap) {
        diag.querySucceeded = true; // a valid empty result
        publishDiagnosticsOnly();
        return;
    }
    if (begin < 0x1000u || end < begin || cap < end ||
        (end - begin) % sizeof(DistanceSortedActor) != 0) {
        publishDiagnosticsOnly();
        return;
    }
    const std::size_t actorCount = (end - begin) / sizeof(DistanceSortedActor);
    if (actorCount > 4096) {
        diag.actorCount = actorCount;
        publishDiagnosticsOnly();
        return;
    }
    diag.querySucceeded = true;
    diag.actorCount = actorCount;

    std::vector<PlayerModeEntry> next;
    std::unordered_set<void*> seen;
    seen.reserve(actorCount);
    for (std::size_t index = 0; index < actorCount; ++index) {
        void* actor = actors.begin[index].mActor;
        if (!actor || actor == localPlayer || !seen.emplace(actor).second) continue;

        bool isPlayer = false;
        try {
            isPlayer = g_actorIsPlayer(actor);
        } catch (...) {
            continue;
        }
        if (!isPlayer) continue;
        ++diag.playerCount;

        std::int32_t gameType = -1;
        const bool hasGameType =
            bedrocktools::core::gamehooks::tryGetPlayerGameType(actor, gameType);
        const bool hasExactGameType = hasGameType && gameType >= 0 && gameType <= 3;
        std::int32_t predicateMode = -1;
        const bool hasPredicateMode = !hasExactGameType &&
            bedrocktools::core::gamehooks::tryGetPlayerCreativeSpectator(actor, predicateMode);
        const bool hasCombatCreativeHint = !hasExactGameType && !hasPredicateMode &&
            bedrocktools::core::gamehooks::tryGetPlayerNoDamageCreativeHint(actor);
        bool attackable = true;
        const bool hasAttackability = !hasExactGameType && !hasPredicateMode &&
            !hasCombatCreativeHint &&
            bedrocktools::core::gamehooks::tryIsActorAttackableGamemode(actor, attackable);
        const bool inferredSpectator = !hasExactGameType && !hasPredicateMode &&
            !hasCombatCreativeHint && hasSpectatorNoclipEvidence(actor);

        std::string mode;
        bool inferred = false;
        if (hasExactGameType) {
            mode = modeName(gameType);
        } else if (hasPredicateMode && (predicateMode == 1 || predicateMode == 3)) {
            mode = modeName(predicateMode);
        } else if (inferredSpectator) {
            mode = "Spectator*";
            inferred = true;
        } else if (hasCombatCreativeHint) {
            mode = "Creative*";
            inferred = true;
        } else if (hasAttackability) {
            inferred = true;
            mode = attackable ? "Survival/Adventure*" : "Creative/Spectator*";
        } else {
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
    const char* enumerator = !m_diagnostics.actorQueryReady ? "query:OFF" :
        !m_diagnostics.querySucceeded ? "query:ERR" : "actors:OK";
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
            : (!m_diagnostics.actorQueryReady || !m_diagnostics.querySucceeded ||
               !m_diagnostics.playerClassifierReady)
                ? "Nearby-player query unavailable" : "No remote players found";
        commands.push_back(empty);
    }
    ::submitDrawCommands(moduleId, commands);
}
