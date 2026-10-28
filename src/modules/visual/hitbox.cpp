#include "hitbox.hpp"
#include "esp.hpp"
#include "oreesp.hpp"
#include "core/GameHooks.hpp"
#include "core/Diagnostics.hpp"
#include "../ModuleRegistry.hpp"
#include "../player/aimassist.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <string>
#include <cstring>
#include <vector>
#include <array>
#include <utility>

typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator, void* material, char* pad);

typedef void* (*HitResult_getEntity_t)(void* hitResult);
typedef bool (*Actor_isPlayer_t)(void* actor);
typedef bool (*Actor_isInvisible_t)(void* actor);
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

typedef ActorVec (*Actor_fetchNearbyActorsSorted_t)(void* actor, void* extent, int actorType);

struct HashedString {
    uint64_t mStrHash;
    std::string mStr;
    mutable const HashedString* mLastMatch;

    HashedString() : mStrHash(0), mStr(), mLastMatch(nullptr) {}

    explicit HashedString(const char* str) : mLastMatch(nullptr) {
        mStr = str ? str : "";
        mStrHash = computeHash(mStr);
    }

private:
    static uint64_t computeHash(const std::string& str) {
        if (str.empty()) return 0;
        constexpr uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr uint64_t kPrime = 0x100000001B3ULL;
        uint64_t hash = kOffset;
        for (char ch : str)
            hash = static_cast<uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        return hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;

    MaterialPtr(MaterialPtr&& other) noexcept
        : sharedPtrData{other.sharedPtrData[0], other.sharedPtrData[1]} {
        other.sharedPtrData[0] = nullptr;
        other.sharedPtrData[1] = nullptr;
    }

    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            sharedPtrData[0] = other.sharedPtrData[0];
            sharedPtrData[1] = other.sharedPtrData[1];
            other.sharedPtrData[0] = nullptr;
            other.sharedPtrData[1] = nullptr;
        }
        return *this;
    }

    ~MaterialPtr() {}

    explicit operator bool() const {
        return sharedPtrData[0] != nullptr;
    }
};

static uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; i++) {
        uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;

        if ((insn & 0x9F000000) == 0x90000000) {
            uintptr_t page = ((uintptr_t)&insns[i] & ~0xFFFULL)
                           + ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);

            for (size_t j = i + 1; j < count; j++) {
                uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            int64_t imm = (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return (uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

static HitboxModule* g_hitboxMod = nullptr;

static Tessellator_begin_t                s_tessBegin = nullptr;
static Tessellator_color_t                s_tessColor = nullptr;
static Tessellator_vertex_t               s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static HitResult_getEntity_t              s_hitResultGetEntity = nullptr;
static Actor_isPlayer_t                   s_actorIsPlayer = nullptr;
static Actor_isInvisible_t                s_actorIsInvisible = nullptr;
static Actor_fetchNearbyActorsSorted_t    s_actorFetchNearby = nullptr;

static MaterialPtr s_matSelection;
// Existing vanilla RenderDragon material whose source definition disables depth testing.
static MaterialPtr s_matThroughWalls;
static const char* s_throughWallsMaterialName = nullptr;
static bool s_throughWallsMaterialChecked = false;
static uintptr_t    s_renderMaterialGroup = 0;

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3);

static bedrocktools::sdk::Vec3 g_playerPos = {0.f, 0.f, 0.f};
static void* g_localPlayerPtr = nullptr;

struct AABB {
    bedrocktools::sdk::Vec3 min;
    bedrocktools::sdk::Vec3 max;
};
static bool validRenderBounds(const AABB& b) {
 return std::isfinite(b.min.x)&&std::isfinite(b.min.y)&&std::isfinite(b.min.z)&&
        std::isfinite(b.max.x)&&std::isfinite(b.max.y)&&std::isfinite(b.max.z)&&
        b.max.x>b.min.x&&b.max.y>b.min.y&&b.max.z>b.min.z;
}
static std::atomic_bool espLoggedLocal{false},espLoggedRenderer{false},espLoggedContext{false},
 espLoggedColor{false},espLoggedQuery{false},espLoggedSnapshot{false},espLoggedEmptyActors{false},
 espLoggedEmptyPlayerQuery{false},espLoggedBounds{false},espLoggedOutline{false},
 espLoggedOverlayMaterial{false},espLoggedOverlayUnavailable{false};
static void espLogOnce(std::atomic_bool& once,bedrocktools::diagnostics::Level level,const char* msg) {
 if(!once.exchange(true,std::memory_order_acq_rel))bedrocktools::diagnostics::log(level,"ESP",msg);
}

static void s_hitboxTickCallback(void* _this) {
    if (!_this) return;
    g_localPlayerPtr = _this;
    uintptr_t svc = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
    if (svc != 0) {
        g_playerPos = *(bedrocktools::sdk::Vec3*)svc;
    }
}

static MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};

    HashedString hs(name);

    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[2]) return {};

    using getMat_t = MaterialPtr(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[2])((void*)s_renderMaterialGroup, &hs);
}

static void ensureMaterials() {
    if (!s_renderMaterialGroup) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning, "Ore ESP",
            "material_group", "missing", "render material group is unresolved; material lookups will retry");
        return;
    }

    if (!s_matSelection) {
        s_matSelection = getMaterial("selection_box");
        bedrocktools::diagnostics::probe(s_matSelection ? bedrocktools::diagnostics::Level::Info : bedrocktools::diagnostics::Level::Warning,
            "Ore ESP", "selection_material", s_matSelection ? "resolved" : "missing",
            s_matSelection ? "selection_box ready" : "selection_box unavailable; using LevelRendererPlayer fallback");
    }

    if (!s_throughWallsMaterialChecked) {
        // ui3D.material is loaded from common.json. Its base name_tag material
        // uses depthFunc=Always and DisableDepthWrite, so it is not gated by Fancy
        // graphics and cannot occlude later world draws with its own depth.
        s_matThroughWalls = getMaterial("name_tag");
        if (s_matThroughWalls) {
            s_throughWallsMaterialName = "name_tag";
        } else {
            // Hologram materials are alternatives when the active pack loads them.
            s_matThroughWalls = getMaterial("reality_frame_skyfill");
            if (s_matThroughWalls) {
                s_throughWallsMaterialName = "reality_frame_skyfill";
            } else {
                s_matThroughWalls = getMaterial("hologram_hit_effect");
                if (s_matThroughWalls)
                    s_throughWallsMaterialName = "hologram_hit_effect";
            }
        }
        s_throughWallsMaterialChecked = true;
        bedrocktools::diagnostics::probe(
            s_matThroughWalls ? bedrocktools::diagnostics::Level::Info : bedrocktools::diagnostics::Level::Warning,
            "Ore ESP", "through_walls_material",
            s_matThroughWalls ? s_throughWallsMaterialName : "unresolved",
            s_matThroughWalls
                ? "depth-test-disabled RenderDragon material resolved"
                : "no loaded vanilla depth-test-disabled material; Ore ESP remains depth-tested");
    }
}

static AABB getActorAABB(void* actor) {
 const AABB empty{{0,0,0},{0,0,0}};const auto addr=reinterpret_cast<std::uintptr_t>(actor);
 if(addr<=0x1000)return empty;
 const auto state=*reinterpret_cast<std::uintptr_t*>(addr+bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
 if(state<=0x1000)return empty;
 const auto component=*reinterpret_cast<std::uintptr_t*>(addr+bedrocktools::sdk::offsets::Actor::mStateVectorComponent+
    bedrocktools::sdk::offsets::BuiltInActorComponents::mAABBShapeComponent);
 if(component<=0x1000)return empty;
 AABB aabb{};std::memcpy(&aabb,reinterpret_cast<const void*>(component+
    bedrocktools::sdk::offsets::AABBShapeComponent::mAABB),sizeof(aabb));
 return validRenderBounds(aabb)?aabb:empty;
}

static bedrocktools::sdk::Vec2 getActorRotation(void* actor) {
    bedrocktools::sdk::Vec2 rot = {0.f, 0.f};
    uintptr_t actorAddr = (uintptr_t)actor;
    uintptr_t rotComp = *(uintptr_t*)(actorAddr + bedrocktools::sdk::offsets::Actor::mActorRotationComponent);
    if (rotComp) {
        rot = *(bedrocktools::sdk::Vec2*)rotComp;
    }
    return rot;
}

static bool hasCategory(void* actor, uint32_t categoryBit) {
    uintptr_t actorAddr = (uintptr_t)actor;
    uint32_t categories = *(uint32_t*)(actorAddr + bedrocktools::sdk::offsets::Actor::mCategories);
    return (categories & categoryBit) != 0;
}



static void updateEspOffscreenTargets(EspModule* esp, void* localPlayer,
                                          const bedrocktools::sdk::Vec3& localPosition,
                                          const ActorVec& actors, bool actorSnapshotValid,
                                          bool actorSnapshotEmpty, float targetRange) {
    if (!esp || !localPlayer) return;
    esp->beginOffscreenFrame(localPosition, getActorRotation(localPlayer));
    if (!esp->enabled || !esp->showOffscreenIndicators ||
        !s_actorFetchNearby || !s_actorIsPlayer) return;

    const bool empty = actorSnapshotEmpty;
    const bool valid = actorSnapshotValid;
    if (!valid) {
        espLogOnce(espLoggedSnapshot, bedrocktools::diagnostics::Level::Warning,
                   "offscreen_actor_snapshot=malformed");
        return;
    }
    if (empty) {
        espLogOnce(espLoggedEmptyPlayerQuery, bedrocktools::diagnostics::Level::Warning,
                   "offscreen_actor_snapshot=empty; no nearby actors returned");
        return;
    }

    bool foundPlayer = false;
    for (DistanceSortedActor* it = actors.begin; it < actors.end; ++it) {
        void* actor = it->mActor;
        if (!actor || reinterpret_cast<std::uintptr_t>(actor) <= 0x1000 ||
            actor == localPlayer || !std::isfinite(it->mDistance) ||
            it->mDistance < 0.0f || it->mDistance > targetRange ||
            !s_actorIsPlayer(actor)) continue;
        foundPlayer = true;
        if (s_actorIsInvisible && s_actorIsInvisible(actor) && !esp->showInvisible) continue;
        AABB bounds = getActorAABB(actor);
        if (!validRenderBounds(bounds)) continue;
        if (auto* expander = static_cast<HitboxExpanderModule*>(
                ModuleRegistry::get().find("bedrocktools.Hitbox Expander"))) {
            const auto nativeBounds = expander->nativeBoundsForActor(actor, {bounds.min, bounds.max});
            bounds = {nativeBounds.min, nativeBounds.max};
        }
        const bedrocktools::sdk::Vec3 center{
            (bounds.min.x + bounds.max.x) * 0.5f,
            (bounds.min.y + bounds.max.y) * 0.5f,
            (bounds.min.z + bounds.max.z) * 0.5f
        };
        esp->considerOffscreenCandidate(center, it->mDistance);
    }
    if (!foundPlayer) {
        espLogOnce(espLoggedEmptyPlayerQuery, bedrocktools::diagnostics::Level::Info,
                   "offscreen_actor_snapshot=valid_but_no_players_in_range");
    }
}

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }

    auto* expander = static_cast<HitboxExpanderModule*>(
        ModuleRegistry::get().find("bedrocktools.Hitbox Expander"));
    auto* esp = static_cast<EspModule*>(ModuleRegistry::get().find("bedrocktools.ESP"));
    auto* oreEsp = static_cast<OreEspModule*>(ModuleRegistry::get().find("bedrocktools.Ore ESP"));
    const bool espEnabled = esp && esp->enabled;
    // Require the complete effective state, not just the cached enabled flag.
    // The shared renderer hook can remain installed while individual modules toggle.
    const bool oreEspEnabled = oreEsp && oreEsp->enabled &&
                               oreEsp->masterEnabled && oreEsp->keybindActive;
    const bool nativeHitboxesEnabled = g_hitboxMod && g_hitboxMod->enabled;
    const bool showExpandedHitboxes = expander && expander->shouldShowHitbox();
    const bool actorVisualsEnabled = nativeHitboxesEnabled || showExpandedHitboxes || espEnabled;
    const bool overlayVisualsEnabled = actorVisualsEnabled || oreEspEnabled;
    if (!overlayVisualsEnabled) return;
    if (!g_localPlayerPtr) { espLogOnce(espLoggedLocal,bedrocktools::diagnostics::Level::Warning,"render_gate=missing_local_player"); return; }

    // Fetch the native actor vector once per RenderLevel call and reuse it for both
    // off-screen HUD targeting and 3D ESP/Hitbox rendering.
    const float espRange = espEnabled ? std::clamp(esp->range, 8.0f, 80.0f) : 30.0f;
    const float queryRange = actorVisualsEnabled ? std::max(30.0f, espRange) : 0.0f;
    ActorVec actors{};
    bool actorSnapshotValid = false;
    bool actorSnapshotEmpty = true;
    if (actorVisualsEnabled && s_actorFetchNearby) {
        bedrocktools::sdk::Vec3 extent{queryRange, queryRange, queryRange};
        actors = s_actorFetchNearby(g_localPlayerPtr, &extent, 1);
        const auto begin = reinterpret_cast<std::uintptr_t>(actors.begin);
        const auto end = reinterpret_cast<std::uintptr_t>(actors.end);
        actorSnapshotEmpty = begin == 0 && end == 0;
        actorSnapshotValid = actorSnapshotEmpty ||
            (begin > 0x1000 && end >= begin &&
             (end - begin) % sizeof(DistanceSortedActor) == 0 &&
             (end - begin) / sizeof(DistanceSortedActor) <= 8192);
    }
    if (espEnabled) {
        updateEspOffscreenTargets(esp, g_localPlayerPtr, g_playerPos,
                                  actors, actorSnapshotValid, actorSnapshotEmpty, espRange);
    }
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) { if(overlayVisualsEnabled)espLogOnce(espLoggedRenderer,bedrocktools::diagnostics::Level::Warning,"render_gate=missing_tessellator_or_mesh_function"); return; }
    if (!screenContext || reinterpret_cast<std::uintptr_t>(screenContext)<0x1000) { if(overlayVisualsEnabled)espLogOnce(espLoggedContext,bedrocktools::diagnostics::Level::Warning,"render_gate=screen_context_unavailable"); return; }

    uintptr_t tessellatorPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (!tessellatorPtr || tessellatorPtr < 0x1000) { if(overlayVisualsEnabled)espLogOnce(espLoggedRenderer,bedrocktools::diagnostics::Level::Warning,"render_gate=tessellator_unavailable"); return; }
    void* tessellator = (void*)tessellatorPtr;

    uintptr_t lrpPtr = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) { if(overlayVisualsEnabled)espLogOnce(espLoggedRenderer,bedrocktools::diagnostics::Level::Warning,"render_gate=level_renderer_player_unavailable"); return; }

    float camX = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    float camY = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
    float camZ = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    ensureMaterials();

    void* matInner = s_matSelection ? (void*)&s_matSelection
                                    : (void*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    uintptr_t colorHolderPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    float* colorHolder = nullptr;
    float savedColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (colorHolderPtr > 0x1000) {
        colorHolder = reinterpret_cast<float*>(colorHolderPtr);
        savedColor[0] = colorHolder[0];
        savedColor[1] = colorHolder[1];
        savedColor[2] = colorHolder[2];
        savedColor[3] = colorHolder[3];
        colorHolder[0] = 1.0f;
        colorHolder[1] = 1.0f;
        colorHolder[2] = 1.0f;
        colorHolder[3] = 1.0f;
    } else {
        espLogOnce(espLoggedColor, bedrocktools::diagnostics::Level::Warning,
                   "render_gate=color_holder_unavailable; continuing with explicit vertex colors");
    }

    auto drawLines = [&](const std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>>& lines, uint32_t color, bool throughWalls = false) {
        if (lines.empty()) return;
        float r = ((color >> 16) & 0xFF) / 255.0f;
        float g = ((color >>  8) & 0xFF) / 255.0f;
        float b = ((color      ) & 0xFF) / 255.0f;
        float a = ((color >> 24) & 0xFF) / 255.0f;

                // Bedrock mce::PrimitiveMode::LineList is 4 (QuadList=1, TriangleList=2, TriangleStrip=3).
        s_tessBegin(tessellator, nullptr, 4, static_cast<int>(lines.size() * 2), 0);
        s_tessColor(tessellator, r, g, b, a);

        for (const auto& line : lines) {
            bedrocktools::sdk::Vec3 p1 = line.first;
            bedrocktools::sdk::Vec3 p2 = line.second;
            p1.x -= camX; p1.y -= camY; p1.z -= camZ;
            p2.x -= camX; p2.y -= camY; p2.z -= camZ;

            s_tessVertex(tessellator, p1.x, p1.y, p1.z);
            s_tessVertex(tessellator, p2.x, p2.y, p2.z);
        }

        char pad[0x58];
        memset(pad, 0, sizeof(pad));
        // Use a native RenderDragon material with DisableDepthTest instead of
        // mutating GL state before submission (the material pipeline overrides it).
        const bool useThroughWallsMaterial = throughWalls && s_matThroughWalls && colorHolder;
        void* materialForBatch = useThroughWallsMaterial
            ? static_cast<void*>(&s_matThroughWalls)
            : matInner;
        float previousBatchColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        if (useThroughWallsMaterial) {
            previousBatchColor[0] = colorHolder[0];
            previousBatchColor[1] = colorHolder[1];
            previousBatchColor[2] = colorHolder[2];
            previousBatchColor[3] = colorHolder[3];
            // current_color.fragment reads ScreenContext's CURRENT_COLOR constant.
            colorHolder[0] = r;
            colorHolder[1] = g;
            colorHolder[2] = b;
            colorHolder[3] = a;
            espLogOnce(espLoggedOverlayMaterial, bedrocktools::diagnostics::Level::Info,
                       "ore_overlay=material; native DisableDepthTest material used");
        } else if (throughWalls) {
            espLogOnce(espLoggedOverlayUnavailable, bedrocktools::diagnostics::Level::Warning,
                       "ore_overlay=depth_tested_fallback; no compatible no-depth material or color holder");
        }

        s_renderMesh(screenContext, tessellator, materialForBatch, pad);

        if (useThroughWallsMaterial) {
            colorHolder[0] = previousBatchColor[0];
            colorHolder[1] = previousBatchColor[1];
            colorHolder[2] = previousBatchColor[2];
            colorHolder[3] = previousBatchColor[3];
        }
    };

    auto drawBox = [&](const AABB& aabb, uint32_t color, bool throughWalls, float thickness) {
        // RenderLevel may visit many actors every frame. Reuse this buffer.
        using Line = std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>;
        static std::vector<Line> lines;
        lines.clear();
        if (lines.capacity() < 60) lines.reserve(60);
        const bedrocktools::sdk::Vec3 mn = aabb.min;
        const bedrocktools::sdk::Vec3 mx = aabb.max;

        lines.push_back({{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}});
        lines.push_back({{mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}});
        lines.push_back({{mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z}});
        lines.push_back({{mn.x, mn.y, mx.z}, {mn.x, mn.y, mn.z}});
        lines.push_back({{mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}});
        lines.push_back({{mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}});
        lines.push_back({{mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}});
        lines.push_back({{mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z}});
        lines.push_back({{mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}});
        lines.push_back({{mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}});
        lines.push_back({{mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}});
        lines.push_back({{mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}});

        // Native line-width controls are unreliable across RenderDragon devices.
        // Approximate thickness with parallel strokes offset around each box edge.
        const float spread = std::clamp(thickness, 0.0f, 0.10f);
        if (spread > 0.0001f) {
            for (std::size_t i = 0; i < 12; ++i) {
                const Line edge = lines[i];
                const auto& a = edge.first;
                const auto& b = edge.second;
                const float dx = std::fabs(b.x - a.x);
                const float dy = std::fabs(b.y - a.y);
                const float dz = std::fabs(b.z - a.z);
                auto addOffset = [&](float ox, float oy, float oz) {
                    lines.push_back({{a.x + ox, a.y + oy, a.z + oz},
                                     {b.x + ox, b.y + oy, b.z + oz}});
                };
                if (dx >= dy && dx >= dz) {
                    addOffset(0, spread, 0); addOffset(0, -spread, 0);
                    addOffset(0, 0, spread); addOffset(0, 0, -spread);
                } else if (dy >= dx && dy >= dz) {
                    addOffset(spread, 0, 0); addOffset(-spread, 0, 0);
                    addOffset(0, 0, spread); addOffset(0, 0, -spread);
                } else {
                    addOffset(spread, 0, 0); addOffset(-spread, 0, 0);
                    addOffset(0, spread, 0); addOffset(0, -spread, 0);
                }
            }
        }
        drawLines(lines, color, throughWalls);
    };

    if (oreEspEnabled && oreEsp) {
        static std::vector<OreEspModule::Outline> outlines;
        outlines.clear();
        oreEsp->getRenderSnapshot(g_playerPos, outlines);
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info, "Ore ESP",
            "renderer_hook", "entered", "shared RenderLevel hook reached with Ore ESP enabled");
        bedrocktools::diagnostics::probe(outlines.empty() ? bedrocktools::diagnostics::Level::Debug : bedrocktools::diagnostics::Level::Info,
            "Ore ESP", "render_snapshot", outlines.empty() ? "empty" : "nonempty",
            "cached outlines submitted=" + std::to_string(outlines.size()));
        using Line = std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>;
        static std::array<std::vector<Line>, 12> linesByKind;
        std::array<std::size_t, 12> oreCounts{};
        for (const auto& ore : outlines) {
            const auto kindIndex = static_cast<std::size_t>(ore.kind);
            if (kindIndex > 0 && kindIndex < oreCounts.size()) ++oreCounts[kindIndex];
        }
        // Reserve once per ore kind. Calling reserve(size + 12) for every ore
        // can force repeated reallocations and quadratic copying as the cache grows.
        for (std::size_t kindIndex = 1; kindIndex < linesByKind.size(); ++kindIndex) {
            auto& lines = linesByKind[kindIndex];
            lines.clear();
            const std::size_t required = oreCounts[kindIndex] * 60;
            if (lines.capacity() < required) lines.reserve(required);
        }
        for (const auto& ore : outlines) {
            const auto kindIndex = static_cast<std::size_t>(ore.kind);
            if (kindIndex == 0 || kindIndex >= linesByKind.size()) continue;
            // Expand slightly outside the block to avoid z-fighting with coplanar faces.
            constexpr float outlineExpansion = 0.006f;
            const float x0 = static_cast<float>(ore.position.x) - outlineExpansion;
            const float y0 = static_cast<float>(ore.position.y) - outlineExpansion;
            const float z0 = static_cast<float>(ore.position.z) - outlineExpansion;
            const float x1 = static_cast<float>(ore.position.x + 1) + outlineExpansion;
            const float y1 = static_cast<float>(ore.position.y + 1) + outlineExpansion;
            const float z1 = static_cast<float>(ore.position.z + 1) + outlineExpansion;
            auto& lines = linesByKind[kindIndex];
            // Draw the original 12 edges plus four parallel strokes around each edge.
            // This increases visible world-space thickness without relying on driver line-width support.
            const float t = std::clamp(oreEsp->outlineThickness, 0.01f, 0.10f);
            auto thickEdge = [&](bedrocktools::sdk::Vec3 a, bedrocktools::sdk::Vec3 b,
                                 bedrocktools::sdk::Vec3 offA, bedrocktools::sdk::Vec3 offB) {
                lines.push_back({a, b});
                lines.push_back({{a.x + offA.x, a.y + offA.y, a.z + offA.z},
                                 {b.x + offA.x, b.y + offA.y, b.z + offA.z}});
                lines.push_back({{a.x - offA.x, a.y - offA.y, a.z - offA.z},
                                 {b.x - offA.x, b.y - offA.y, b.z - offA.z}});
                lines.push_back({{a.x + offB.x, a.y + offB.y, a.z + offB.z},
                                 {b.x + offB.x, b.y + offB.y, b.z + offB.z}});
                lines.push_back({{a.x - offB.x, a.y - offB.y, a.z - offB.z},
                                 {b.x - offB.x, b.y - offB.y, b.z - offB.z}});
            };
            // Horizontal edges use the two perpendicular axes; vertical edges use X/Z.
            thickEdge({x0,y0,z0},{x1,y0,z0},{0,0,t},{0,t,0});
            thickEdge({x1,y0,z0},{x1,y0,z1},{t,0,0},{0,t,0});
            thickEdge({x1,y0,z1},{x0,y0,z1},{0,0,t},{0,t,0});
            thickEdge({x0,y0,z1},{x0,y0,z0},{t,0,0},{0,t,0});
            thickEdge({x0,y1,z0},{x1,y1,z0},{0,0,t},{0,t,0});
            thickEdge({x1,y1,z0},{x1,y1,z1},{t,0,0},{0,t,0});
            thickEdge({x1,y1,z1},{x0,y1,z1},{0,0,t},{0,t,0});
            thickEdge({x0,y1,z1},{x0,y1,z0},{t,0,0},{0,t,0});
            thickEdge({x0,y0,z0},{x0,y1,z0},{t,0,0},{0,0,t});
            thickEdge({x1,y0,z0},{x1,y1,z0},{t,0,0},{0,0,t});
            thickEdge({x1,y0,z1},{x1,y1,z1},{t,0,0},{0,0,t});
            thickEdge({x0,y0,z1},{x0,y1,z1},{t,0,0},{0,0,t});
        }
        for (std::size_t kindIndex = 1; kindIndex < linesByKind.size(); ++kindIndex) {
            auto& lines = linesByKind[kindIndex];
            if (lines.empty()) continue;
            drawLines(lines, bedaim::xray::colorForOreKind(static_cast<bedaim::xray::OreKind>(kindIndex)), true);
        }
    }

    // Reference-style reticle above each ESP player. It lives in world space and
    // uses only that actor's current AABB, so Aim Assist never gates the ring.
    auto drawTargetRing = [&](const AABB& aabb) {
        // Anchor the marker to a stable point above the actor's feet. BedrockBounds
        // can enlarge max.y, so using the AABB midpoint makes the marker rise as the
        // expanded hitbox grows. Keep the marker's position and radius independent of
        // the expanded collision bounds.
        const bedrocktools::sdk::Vec3 center{
            (aabb.min.x + aabb.max.x) * 0.5f,
            aabb.min.y + 0.90f + (esp ? esp->targetYOffset : 0.0f),
            (aabb.min.z + aabb.max.z) * 0.5f
        };

        float nx = camX - center.x;
        float ny = camY - center.y;
        float nz = camZ - center.z;
        const float nLen = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (!std::isfinite(nLen) || nLen < 0.001f) return;
        nx /= nLen; ny /= nLen; nz /= nLen;

        float rx = nz, ry = 0.0f, rz = -nx;
        const float rLen = std::sqrt(rx*rx + ry*ry + rz*rz);
        if (rLen < 0.001f) { rx = 1.0f; ry = 0.0f; rz = 0.0f; }
        else { rx /= rLen; ry /= rLen; rz /= rLen; }
        const float ux = ny*rz - nz*ry;
        const float uy = nz*rx - nx*rz;
        const float uz = nx*ry - ny*rx;

        constexpr double tau = 6.2831853071795864769;
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const double animationEnabled = (esp && esp->animatedIndicators) ? 1.0 : 0.0;
        const double targetSpeed = esp ? std::clamp(esp->targetSpeed, 0.1f, 4.0f) : 1.0;
        const float outerPhase = static_cast<float>(std::fmod(seconds * 0.78 * animationEnabled * targetSpeed, tau));
        const float innerPhase = static_cast<float>(std::fmod(-seconds * 1.12 * animationEnabled * targetSpeed, tau));
        const float sizeScale = esp ? std::clamp(esp->indicatorSize / 48.0f, 0.5f, 2.0f) : 1.0f;
        const float radius = std::clamp(0.42f * sizeScale, 0.22f, 0.84f);
        using RingLine = std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>;
        // Reuse ring buffers across actors and frames; this path runs every frame.
        static std::vector<RingLine> outerLines;
        static std::vector<RingLine> innerLines;
        outerLines.clear();
        innerLines.clear();
        if (outerLines.capacity() < 88) outerLines.reserve(88);
        if (innerLines.capacity() < 76) innerLines.reserve(76);
        auto pointAt = [&](float r, float angle) {
            return bedrocktools::sdk::Vec3{
                center.x + rx * std::cos(angle) * r + ux * std::sin(angle) * r,
                center.y + ry * std::cos(angle) * r + uy * std::sin(angle) * r,
                center.z + rz * std::cos(angle) * r + uz * std::sin(angle) * r
            };
        };

        // Four rotating breaks make the outer ring read as a sharp reticle, not a halo.
        constexpr int outerSegments = 64;
        for (int i = 0; i < outerSegments; ++i) {
            if ((i % 16) >= 12) continue;
            const float a0 = static_cast<float>(tau * i / outerSegments) + outerPhase;
            const float a1 = static_cast<float>(tau * (i + 1) / outerSegments) + outerPhase;
            outerLines.push_back({pointAt(radius, a0), pointAt(radius, a1)});
        }

        // Four angular arrowheads rotate with the outer segments and match the user's PNG.
        for (int i = 0; i < 4; ++i) {
            const float angle = static_cast<float>(tau * i / 4.0) + outerPhase;
            const auto tip = pointAt(radius * 0.78f, angle);
            const auto base = pointAt(radius * 1.18f, angle);
            const float wing = radius * 0.18f;
            const float tangentX = -rx * std::sin(angle) + ux * std::cos(angle);
            const float tangentY = -ry * std::sin(angle) + uy * std::cos(angle);
            const float tangentZ = -rz * std::sin(angle) + uz * std::cos(angle);
            const bedrocktools::sdk::Vec3 wingA{
                base.x + tangentX * wing, base.y + tangentY * wing, base.z + tangentZ * wing};
            const bedrocktools::sdk::Vec3 wingB{
                base.x - tangentX * wing, base.y - tangentY * wing, base.z - tangentZ * wing};
            outerLines.push_back({tip, wingA});
            outerLines.push_back({wingA, wingB});
            outerLines.push_back({wingB, tip});
        }

        // Inner segmented arcs counter-rotate slowly, creating motion without flashing.
        constexpr int middleSegments = 48;
        for (int i = 0; i < middleSegments; ++i) {
            if ((i % 12) >= 10) continue;
            const float a0 = static_cast<float>(tau * i / middleSegments) + innerPhase;
            const float a1 = static_cast<float>(tau * (i + 1) / middleSegments) + innerPhase;
            innerLines.push_back({pointAt(radius * 0.70f, a0), pointAt(radius * 0.70f, a1)});
        }
        constexpr int innerSegments = 32;
        for (int i = 0; i < innerSegments; ++i) {
            if ((i % 8) == 7) continue;
            const float a0 = static_cast<float>(tau * i / innerSegments) - outerPhase * 0.45f;
            const float a1 = static_cast<float>(tau * (i + 1) / innerSegments) - outerPhase * 0.45f;
            innerLines.push_back({pointAt(radius * 0.39f, a0), pointAt(radius * 0.39f, a1)});
        }

        // Increase visible ring thickness using near-parallel world-space strokes.
        // The target's native AABB is supplied by renderActor so BedrockBounds cannot
        // enlarge the marker along with its expanded collision/query bounds.
        const int ringLayers = esp ? static_cast<int>(std::clamp(std::round(esp->targetThickness), 1.0f, 5.0f)) : 1;
        static std::vector<RingLine> thickOuterLines;
        static std::vector<RingLine> thickInnerLines;
        auto makeThick = [&](const std::vector<RingLine>& source, std::vector<RingLine>& output) {
            output.assign(source.begin(), source.end());
            if (ringLayers <= 1) return;
            const std::size_t needed = source.size() * static_cast<std::size_t>(1 + 2 * (ringLayers - 1));
            if (output.capacity() < needed) output.reserve(needed);
            auto offsetPoint = [&](const bedrocktools::sdk::Vec3& point, float radialOffset) {
                const float vx = point.x - center.x;
                const float vy = point.y - center.y;
                const float vz = point.z - center.z;
                const float length = std::sqrt(vx*vx + vy*vy + vz*vz);
                if (length < 0.001f || length + radialOffset <= 0.001f) return point;
                const float scale = (length + radialOffset) / length;
                return bedrocktools::sdk::Vec3{center.x + vx*scale, center.y + vy*scale, center.z + vz*scale};
            };
            for (const auto& line : source) {
                for (int layer = 1; layer < ringLayers; ++layer) {
                    const float offset = 0.0125f * static_cast<float>(layer);
                    output.push_back({offsetPoint(line.first, offset), offsetPoint(line.second, offset)});
                    output.push_back({offsetPoint(line.first, -offset), offsetPoint(line.second, -offset)});
                }
            }
        };
        makeThick(outerLines, thickOuterLines);
        makeThick(innerLines, thickInnerLines);
        drawLines(thickOuterLines, 0xFF7247FFu, true); // violet, through walls
        drawLines(thickInnerLines, 0xFF405DFFu, true); // electric blue, through walls
    };

    void* selectedEntity = nullptr;
    uintptr_t levelPtr = *(uintptr_t*)((uintptr_t)g_localPlayerPtr + bedrocktools::sdk::offsets::Actor::mLevel);
    if (levelPtr && s_hitResultGetEntity) {
        uintptr_t hitResultWrapper = levelPtr + bedrocktools::sdk::offsets::Level::mHitResultWrapper;
        void* hitResult = (void*)(hitResultWrapper + bedrocktools::sdk::offsets::HitResultWrapper::mHitResult);

        int hitType = *(int*)((uintptr_t)hitResult + bedrocktools::sdk::offsets::HitResult::mType);
        if (hitType == 1) {
            selectedEntity = s_hitResultGetEntity(hitResult);
        }
    }

    bedrocktools::sdk::Vec3 localPos = g_playerPos;
    float dx = camX - localPos.x;
    float dy = camY - (localPos.y + 1.62f);
    float dz = camZ - localPos.z;
    bool isThirdPerson = (dx*dx + dy*dy + dz*dz) > 0.05f;

    auto renderActor = [&](void* ent, bool allowEsp) {
        const bool isPlayer=s_actorIsPlayer&&s_actorIsPlayer(ent);
        AABB aabb=getActorAABB(ent);
        if(!validRenderBounds(aabb)){
            if(espEnabled&&esp->showPlayers&&isPlayer&&ent!=g_localPlayerPtr)
              espLogOnce(espLoggedBounds,bedrocktools::diagnostics::Level::Warning,"actor_bounds=invalid_or_unavailable; skipped ESP outline");
            return;
        }
        const bool wantsEsp=allowEsp&&espEnabled&&esp->showPlayers&&isPlayer&&ent!=g_localPlayerPtr;
        AABB espBounds = aabb;
        if (expander) {
            const auto native = expander->nativeBoundsForActor(ent, {aabb.min, aabb.max});
            espBounds = {native.min, native.max};
        }
        const bool isExpandedTarget=showExpandedHitboxes&&expander&&ent!=g_localPlayerPtr&&expander->expandsType(isPlayer);
        if(wantsEsp){
            std::int32_t gameType=-1;std::uint32_t color=esp->unknownColor;
            if(bedrocktools::core::gamehooks::tryGetPlayerGameType(ent,gameType)){
                switch(gameType){case 0:color=esp->survivalColor;break;case 1:color=esp->creativeColor;break;
                 case 2:color=esp->adventureColor;break;case 3:color=esp->spectatorColor;break;default:color=esp->unknownColor;break;}
            }
            drawBox(espBounds,color,true,esp->outlineThickness);
            if (esp->showLockInAim) drawTargetRing(espBounds);
            espLogOnce(espLoggedOutline,bedrocktools::diagnostics::Level::Info,"outline_submitted=first_player_box; renderer submitted ESP geometry and target ring");
        }
        if (!nativeHitboxesEnabled && !isExpandedTarget) return;
        if (isExpandedTarget) {
            const bedrocktools::sdk::AABB nativeBounds{aabb.min, aabb.max};
            const auto expanded = expander->expandedTargetBounds(isPlayer, nativeBounds);
            aabb = {expanded.min, expanded.max};
        }
        const uint32_t boxColor = g_hitboxMod ? g_hitboxMod->hitboxColor : 0xFF00FFFF;
        // Do not repaint the same native player bounds over an ESP game-mode color.
        // Expanded bounds remain separately visible when Bedrock Bounds is enabled.
        if (!wantsEsp || isExpandedTarget)
            drawBox(aabb, boxColor, false, esp ? esp->hitboxThickness : 0.025f);

        if (!nativeHitboxesEnabled) return;
        if (g_hitboxMod->showEyeLine) {
            float minX = aabb.min.x;
            float maxX = aabb.max.x;
            float minZ = aabb.min.z;
            float maxZ = aabb.max.z;

            float entityHeight = aabb.max.y - aabb.min.y;
            float eyeHeight = aabb.min.y + entityHeight * 0.85f;

            using ActorLine = std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>;
            static std::vector<ActorLine> eyeLines;
            eyeLines.clear();
            if (eyeLines.capacity() < 4) eyeLines.reserve(4);
            eyeLines.push_back({bedrocktools::sdk::Vec3{minX, eyeHeight, minZ}, bedrocktools::sdk::Vec3{maxX, eyeHeight, minZ}});
            eyeLines.push_back({bedrocktools::sdk::Vec3{maxX, eyeHeight, minZ}, bedrocktools::sdk::Vec3{maxX, eyeHeight, maxZ}});
            eyeLines.push_back({bedrocktools::sdk::Vec3{maxX, eyeHeight, maxZ}, bedrocktools::sdk::Vec3{minX, eyeHeight, maxZ}});
            eyeLines.push_back({bedrocktools::sdk::Vec3{minX, eyeHeight, maxZ}, bedrocktools::sdk::Vec3{minX, eyeHeight, minZ}});
            drawLines(eyeLines, g_hitboxMod->eyeLineColor);
        }

        if (g_hitboxMod->showLookLine) {
            bedrocktools::sdk::Vec2 rot = getActorRotation(ent);
            static constexpr float PI = 3.14159265f;
            static constexpr float DEG_TO_RAD = PI / 180.0f;

            float yawR = rot.y * DEG_TO_RAD;
            float pitchR = rot.x * DEG_TO_RAD;
            float dirX = -sinf(yawR) * cosf(pitchR);
            float dirY = -sinf(pitchR);
            float dirZ = cosf(yawR) * cosf(pitchR);

            float entityHeight = aabb.max.y - aabb.min.y;
            float eyeHeight = aabb.min.y + entityHeight * 0.85f;
            float centerX = (aabb.min.x + aabb.max.x) * 0.5f;
            float centerZ = (aabb.min.z + aabb.max.z) * 0.5f;

            bedrocktools::sdk::Vec3 start = {centerX, eyeHeight, centerZ};
            float lineLen = g_hitboxMod->lookLineLength;
            bedrocktools::sdk::Vec3 end = {start.x + dirX * lineLen, start.y + dirY * lineLen, start.z + dirZ * lineLen};

            using ActorLine = std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>;
            static std::vector<ActorLine> lookLines;
            lookLines.clear();
            if (lookLines.capacity() < 1) lookLines.reserve(1);
            lookLines.push_back({start, end});
            drawLines(lookLines, g_hitboxMod->lookLineColor);
        }
    };

    if (nativeHitboxesEnabled && g_hitboxMod->showSelf && isThirdPerson) {
        renderActor(g_localPlayerPtr, false);
    }

    if(s_actorFetchNearby){
        if(!actorSnapshotValid){if(espEnabled)espLogOnce(espLoggedSnapshot,bedrocktools::diagnostics::Level::Warning,"actor_snapshot=malformed; skipped nearby actor list");}
        else if(actorSnapshotEmpty){if(espEnabled)espLogOnce(espLoggedEmptyActors,bedrocktools::diagnostics::Level::Warning,"actor_snapshot=empty; no nearby actors returned by ActorFetchNearbyActorsSorted");}
        else for(DistanceSortedActor* it=actors.begin;it<actors.end;++it){
            void* ent=it->mActor;
            if(!ent||reinterpret_cast<std::uintptr_t>(ent)<=0x1000||ent==g_localPlayerPtr||
               !std::isfinite(it->mDistance)||it->mDistance<0||it->mDistance>queryRange)continue;
            const bool isPlayer=s_actorIsPlayer&&s_actorIsPlayer(ent);
            const bool wantsEsp=espEnabled&&esp->showPlayers&&isPlayer&&it->mDistance<=espRange;
            const bool isEntityCategory=isPlayer||hasCategory(ent,2);
            const bool wantsExpanded=showExpandedHitboxes&&expander&&expander->expandsType(isPlayer)&&isEntityCategory;
            const bool wantsNative=nativeHitboxesEnabled&&(isPlayer?g_hitboxMod->showPlayers:(hasCategory(ent,2)&&g_hitboxMod->showEntities));
            if(!wantsEsp&&!wantsExpanded&&!wantsNative)continue;
            if(s_actorIsInvisible&&s_actorIsInvisible(ent)&&!(wantsEsp&&esp->showInvisible))continue;
            renderActor(ent,wantsEsp);
        }
    }else if(espEnabled){
        espLogOnce(espLoggedQuery,bedrocktools::diagnostics::Level::Warning,"actor_query=signature_unavailable; ESP cannot enumerate nearby players");
    }

    if (colorHolder) {
        colorHolder[0] = savedColor[0];
        colorHolder[1] = savedColor[1];
        colorHolder[2] = savedColor[2];
        colorHolder[3] = savedColor[3];
    }
}

HitboxModule::HitboxModule()
    : Module("Hitbox", "Internal shared renderer for ESP overlays.") {

    showInMenu = false;
    hideInHudEditor = true;

    m_patched = false;
    m_patchTarget = nullptr;
    m_tessBeginAddr = nullptr;
    m_tessColorAddr = nullptr;
    m_tessVertexAddr = nullptr;
    m_renderMaterialGroupAddr = nullptr;
    g_hitboxMod = this;
}

HitboxModule::~HitboxModule() {
    if (g_hitboxMod == this) g_hitboxMod = nullptr;
}

void HitboxModule::onInit() {
    uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (addr != 0) {
        m_patchTarget = (void*)addr;
    }

    uintptr_t tb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    if (tb) { m_tessBeginAddr = (void*)tb; s_tessBegin = (Tessellator_begin_t)tb; }

    uintptr_t tc = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    if (tc) { m_tessColorAddr = (void*)tc; s_tessColor = (Tessellator_color_t)tc; }

    uintptr_t tv = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tv) {
        m_tessVertexAddr = (void*)tv;
        s_tessVertex = (Tessellator_vertex_t)tv;
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info, "ESP",
            "tessellator_vertex", "resolved", "3D vertex routine resolved; ABI is Tessellator* plus x/y/z float arguments");
    } else {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning, "ESP",
            "tessellator_vertex", "unresolved", "TessellatorVertex signature did not match the loaded Minecraft library");
    }

    uintptr_t rm = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (rm) {
        s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm;
    } else {
        uintptr_t rm5 = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
        if (rm5) s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm5;
    }

    uintptr_t rmg = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (rmg) {
        m_renderMaterialGroupAddr = (void*)rmg;
        uintptr_t groupAddr = resolveADRP(reinterpret_cast<uint32_t*>(rmg), 2, 0);
        if (groupAddr) {
            s_renderMaterialGroup = groupAddr + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    uintptr_t hrge = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::HitResultGetEntity);
    if (hrge) s_hitResultGetEntity = (HitResult_getEntity_t)hrge;

    uintptr_t aip = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer);
    if (aip) s_actorIsPlayer = (Actor_isPlayer_t)aip;

    uintptr_t aii = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsInvisible);
    if (aii) s_actorIsInvisible = (Actor_isInvisible_t)aii;

    uintptr_t afn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted);
    if (afn) s_actorFetchNearby = (Actor_fetchNearbyActorsSorted_t)afn;

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_hitboxTickCallback(event.player); });
    std::string missing;
    if(!m_patchTarget)missing+=" RenderLevel";
    if(!s_tessBegin)missing+=" TessellatorBegin";
    if(!s_tessColor)missing+=" TessellatorColor";
    if(!s_tessVertex)missing+=" TessellatorVertex";
    if(!s_renderMesh)missing+=" MeshHelpersRenderMeshImmediately";
    if(!s_actorFetchNearby)missing+=" ActorFetchNearbyActorsSorted";
    if(!s_actorIsPlayer)missing+=" ActorIsPlayer";
    bedrocktools::diagnostics::log(missing.empty()?bedrocktools::diagnostics::Level::Info:bedrocktools::diagnostics::Level::Warning,
      "ESP",missing.empty()?std::string("renderer_dependencies=ready; shared RenderLevel path available"):std::string("renderer_dependencies_missing:")+missing);
    applyPatch();
}

void HitboxModule::applyPatch() {
    if(m_patched||!m_patchTarget)return;
    const auto hook=bedrocktools::hooks::install(m_patchTarget,reinterpret_cast<void*>(_renderLevel_hook),
                                                 reinterpret_cast<void**>(&_renderLevel_orig));
    m_patched=hook!=nullptr;
    bedrocktools::diagnostics::log(m_patched?bedrocktools::diagnostics::Level::Info:bedrocktools::diagnostics::Level::Warning,
      "ESP",m_patched?"renderlevel_hook=installed":"renderlevel_hook=installation_failed");
}

void HitboxModule::ensureRenderHook() {
    if (!m_patchTarget) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning, "ESP",
            "renderlevel_hook=unavailable; RenderLevel signature was not resolved");
        return;
    }
    applyPatch();
}

void HitboxModule::onEnable() {
    ensureRenderHook();
}

void HitboxModule::onDisable() {
}

void HitboxModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    showEntities = j.value("showEntities", showEntities);
    showPlayers = j.value("showPlayers", showPlayers);
    showSelf = j.value("showSelf", showSelf);
    showEyeLine = j.value("showEyeLine", showEyeLine);
    showLookLine = j.value("showLookLine", showLookLine);
    lookLineLength = j.value("lookLineLength", lookLineLength);

    auto parseColor = [&](const std::string& key, uint32_t& outColor) {
        if (j.contains(key)) {
            std::string hexStr = j[key].get<std::string>();
            if (!hexStr.empty() && hexStr[0] == '#') {
                try { outColor = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
            }
        }
    };

    parseColor("hitboxColor", hitboxColor);
    parseColor("eyeLineColor", eyeLineColor);
    parseColor("lookLineColor", lookLineColor);
}

void HitboxModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["showEntities"] = showEntities;
    j["showPlayers"] = showPlayers;
    j["showSelf"] = showSelf;
    j["showEyeLine"] = showEyeLine;
    j["showLookLine"] = showLookLine;
    j["lookLineLength"] = lookLineLength;

    char hexH[12], hexE[12], hexL[12];
    snprintf(hexH, sizeof(hexH), "#%08X", hitboxColor);
    snprintf(hexE, sizeof(hexE), "#%08X", eyeLineColor);
    snprintf(hexL, sizeof(hexL), "#%08X", lookLineColor);

    j["hitboxColor"] = std::string(hexH);
    j["eyeLineColor"] = std::string(hexE);
    j["lookLineColor"] = std::string(hexL);
}