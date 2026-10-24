#include "SpectatorProbe.hpp"
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
#include <mutex>
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
    if (!actor || !g_actorGetStatusFlag || !g_actorIsFlying) return false;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> probeLock(g_spectatorProbeMutex);
    auto& probe = g_spectatorProbes[actor];

    // Aim Assist calls this per tick; sample each actor at most about 6.7 Hz.
    if (probe.hasLastPosition && now - probe.lastSeen < std::chrono::milliseconds(150))
        return probe.consecutiveNoclipSamples >= 2;

    const auto* nativeActor = reinterpret_cast<const bedrocktools::sdk::Actor*>(actor);
    bool invisible = false;
    bool hasCollision = true;
    bool flying = false;
    try {
        invisible = g_actorGetStatusFlag(actor, 5);     // ActorFlags::Invisible
        hasCollision = g_actorGetStatusFlag(actor, 48); // ActorFlags::HasCollision
        flying = g_actorIsFlying(actor);
    } catch (...) {
        probe.lastSeen = now;
        return false;
    }

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

    // Avoid block scans unless the cheap state flags already resemble Spectator.
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

namespace bedaim::spectatorprobe {

bool isSpectatorCandidate(void* actor) {
    // Keep the Aim Assist filter self-contained; the removed GameMode Checker
    // UI module is no longer responsible for initializing these native lookups.
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
    return hasSpectatorNoclipEvidence(actor);
}

} // namespace bedaim::spectatorprobe
