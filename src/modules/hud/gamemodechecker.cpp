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
using GetRuntimeActorListFn = std::vector<void*> (*)(void*);
using ActorIsPlayerFn = bool (*)(void*);
using ActorGetNameTagFn = const std::string& (*)(const void*);
using ActorGetStatusFlagFn = bool (*)(const void*, int);
using ActorIsFlyingFn = bool (*)(const void*);
using BlockSourceIsSolidBlockingBlockFn =
    bool (*)(void*, const bedrocktools::sdk::BlockPos*);

GetRuntimeActorListFn g_getRuntimeActorList = nullptr;
ActorIsPlayerFn g_actorIsPlayer = nullptr;
ActorGetNameTagFn g_actorGetNameTag = nullptr;
ActorGetStatusFlagFn g_actorGetStatusFlag = nullptr;
ActorIsFlyingFn g_actorIsFlying = nullptr;
BlockSourceIsSolidBlockingBlockFn g_isSolidBlockingBlock = nullptr;
GamemodeCheckerModule* g_checker = nullptr;

struct SpectatorProbe {
    bedrocktools::sdk::Vec3 lastPosition{};
    unsigned consecutiveNoclipSamples = 0;
    bool hasLastPosition = false;
    std::chrono::steady_clock::time_point lastSeen{};
};
std::unordered_map<void*, SpectatorProbe> g_spectatorProbes;

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
    if (!actor || !g_actorGetStatusFlag || !g_actorIsFlying) return false;
    const auto* nativeActor = reinterpret_cast<const bedrocktools::sdk::Actor*>(actor);
    bool invisible = false;
    bool hasCollision = true;
    bool flying = false;
    try {
        invisible = g_actorGetStatusFlag(actor, 5);     // ActorFlags::Invisible
        hasCollision = g_actorGetStatusFlag(actor, 48); // ActorFlags::HasCollision
        flying = g_actorIsFlying(actor);
    } catch (...) {
        return false;
    }

    const auto position = nativeActor->position();
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) return false;
    auto& probe = g_spectatorProbes[actor];
    const bool moving = probe.hasLastPosition &&
        (std::abs(position.x - probe.lastPosition.x) > 0.12f ||
         std::abs(position.y - probe.lastPosition.y) > 0.12f ||
         std::abs(position.z - probe.lastPosition.z) > 0.12f);
    probe.lastPosition = position;
    probe.hasLastPosition = true;
    probe.lastSeen = std::chrono::steady_clock::now();

    // Only pay for block probes when the cheap flags already resemble Spectator.
    if (!invisible || hasCollision || !flying || !moving) {
        probe.consecutiveNoclipSamples = 0;
        return false;
    }

    const bool bodyOverlapsSolidBlock = hasSolidBlockInsideBody(actor);
    if (bodyOverlapsSolidBlock) {
        probe.consecutiveNoclipSamples = std::min(probe.consecutiveNoclipSamples + 1u, 5u);
    } else {
        probe.consecutiveNoclipSamples = 0;
    }
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
    g_getRuntimeActorList = reinterpret_cast<GetRuntimeActorListFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList));
    g_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));
    if (!g_actorGetNameTag) {
        g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
            resolveGameSymbol("_ZNK5Actor10getNameTagEv"));
    }
    g_actorGetStatusFlag = reinterpret_cast<ActorGetStatusFlagFn>(
        resolveGameSymbol("_ZNK5Actor13getStatusFlagE10ActorFlags"));
    g_actorIsFlying = reinterpret_cast<ActorIsFlyingFn>(
        resolveGameSymbol("_ZNK6Player8isFlyingEv"));
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

void GamemodeCheckerModule::refresh(void* localPlayer) {
    // Keep native enumeration capped at 5 Hz on mobile.
    if (++m_ticks < 4) return;
    m_ticks = 0;

    // Runtime signatures can resolve after the module is initialized; retry missing
    // lookups on this throttled refresh instead of leaving the overlay permanently empty.
    if (!g_getRuntimeActorList) {
        g_getRuntimeActorList = reinterpret_cast<GetRuntimeActorListFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList));
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
    if (!g_actorGetStatusFlag) {
        g_actorGetStatusFlag = reinterpret_cast<ActorGetStatusFlagFn>(
            resolveGameSymbol("_ZNK5Actor13getStatusFlagE10ActorFlags"));
    }
    if (!g_actorIsFlying) {
        g_actorIsFlying = reinterpret_cast<ActorIsFlyingFn>(
            resolveGameSymbol("_ZNK6Player8isFlyingEv"));
    }
    if (!g_isSolidBlockingBlock) {
        g_isSolidBlockingBlock = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock));
    }
    const auto probeNow = std::chrono::steady_clock::now();
    for (auto it = g_spectatorProbes.begin(); it != g_spectatorProbes.end();) {
        if (probeNow - it->second.lastSeen > std::chrono::seconds(12))
            it = g_spectatorProbes.erase(it);
        else
            ++it;
    }

    GamemodeCheckerDiagnostics diag{};
    diag.actorListReady = g_getRuntimeActorList != nullptr;
    diag.playerClassifierReady = g_actorIsPlayer != nullptr;
    // SDK's field-backed name is the primary route; the native getter is fallback only.
    diag.nameGetterReady = true;

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
        } else if (hasCombatCreativeHint) {
            mode = "Creative*";
            inferred = true;
        } else if (inferredSpectator) {
            mode = "Spectator*";
            inferred = true;
        } else if (hasAttackability) {
            inferred = true;
            if (attackable) mode = "Survival/Adventure*";
            else mode = "Creative/Spectator*";
        } else {
            mode = "Unknown";
        }       if (hasExactGameType) ++diag.exactModeCount;
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
    const char* enumerator = !m_diagnostics.actorListReady ? "actors:OFF" :
        !m_diagnostics.levelReady ? "level:OFF" :
        !m_diagnostics.actorManagerReady ? "manager:OFF" : "actors:OK";
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
               !m_diagnostics.levelReady || !m_diagnostics.actorManagerReady)
                ? "Enumeration path unavailable" : "No remote players found";
        commands.push_back(empty);
    }
    ::submitDrawCommands(moduleId, commands);
}
