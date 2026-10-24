#include "hitbox.hpp"
#include "esp.hpp"
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
#include <algorithm>
#include <atomic>
#include <string>
#include <cstring>
#include <vector>
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

// Bedrock 1.26.51 ActorType values: Mob=1, Player=69.
static constexpr int kActorTypeMob_1_26_51 = 1;
static constexpr int kActorTypePlayer_1_26_51 = 69;

// Match the actual native return type so libc++ destroys each returned vector correctly.
typedef std::vector<DistanceSortedActor> (*Actor_fetchNearbyActorsSorted_t)(
    void* actor, const bedrocktools::sdk::Vec3& extent, int actorType);

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
 espLoggedEmptyPlayerQuery{false},espLoggedBounds{false},espLoggedOutline{false};
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
    if (s_matSelection) return;
    if (!s_renderMaterialGroup) return;

    if (!s_matSelection) s_matSelection = getMaterial("selection_box");
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
    for (const DistanceSortedActor& entry : actors) {
        void* actor = entry.mActor;
        if (!actor || reinterpret_cast<std::uintptr_t>(actor) <= 0x1000 ||
            actor == localPlayer || !std::isfinite(entry.mDistance) ||
            entry.mDistance < 0.0f || entry.mDistance > targetRange ||
            !s_actorIsPlayer(actor)) continue;
        foundPlayer = true;
        if (s_actorIsInvisible && s_actorIsInvisible(actor) && !esp->showInvisible) continue;
        const AABB bounds = getActorAABB(actor);
        if (!validRenderBounds(bounds)) continue;
        const bedrocktools::sdk::Vec3 center{
            (bounds.min.x + bounds.max.x) * 0.5f,
            (bounds.min.y + bounds.max.y) * 0.5f,
            (bounds.min.z + bounds.max.z) * 0.5f
        };
        esp->considerOffscreenCandidate(center, entry.mDistance);
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
    const bool espEnabled = esp && esp->enabled;
    const bool nativeHitboxesEnabled = g_hitboxMod && g_hitboxMod->enabled;
    const bool showExpandedHitboxes = expander && expander->shouldShowHitbox();
    if (!nativeHitboxesEnabled && !showExpandedHitboxes && !espEnabled) return;
    if (!g_localPlayerPtr) { if(espEnabled)espLogOnce(espLoggedLocal,bedrocktools::diagnostics::Level::Warning,"render_gate=missing_local_player"); return; }

    const float espRange = espEnabled ? std::clamp(esp->range, 8.0f, 80.0f) : 30.0f;
    const float queryRange = std::max(30.0f, espRange);
    const bool needPlayerActors =
        (espEnabled && (esp->showPlayers || esp->showOffscreenIndicators)) ||
        (nativeHitboxesEnabled && g_hitboxMod->showPlayers) ||
        (showExpandedHitboxes && expander && expander->expandsType(true));
    const bool needMobActors =
        (nativeHitboxesEnabled && g_hitboxMod->showEntities) ||
        (showExpandedHitboxes && expander && expander->expandsType(false));

    std::vector<DistanceSortedActor> playerActors;
    std::vector<DistanceSortedActor> mobActors;
    bool playerSnapshotValid = true, playerSnapshotEmpty = true;
    bool mobSnapshotValid = true, mobSnapshotEmpty = true;
    auto fetchActorSnapshot = [&](int actorType, std::vector<DistanceSortedActor>& out,
                                  bool& valid, bool& empty) {
        bedrocktools::sdk::Vec3 extent{queryRange, queryRange, queryRange};
        out = s_actorFetchNearby(g_localPlayerPtr, extent, actorType);
        empty = out.empty();
        valid = out.size() <= 8192;
    };

    if (s_actorFetchNearby) {
        // ActorType::Player and ActorType::Mob are different filters; never use
        // the Mob vector as the source for ESP/target-ring player candidates.
        if (needPlayerActors) {
            fetchActorSnapshot(kActorTypePlayer_1_26_51, playerActors,
                               playerSnapshotValid, playerSnapshotEmpty);
        }
        if (needMobActors) {
            fetchActorSnapshot(kActorTypeMob_1_26_51, mobActors,
                               mobSnapshotValid, mobSnapshotEmpty);
        }
    } else {
        playerSnapshotValid = false;
        mobSnapshotValid = false;
    }
    if (espEnabled) {
        updateEspOffscreenTargets(esp, g_localPlayerPtr, g_playerPos,
                                  playerActors, playerSnapshotValid, playerSnapshotEmpty, espRange);
    }
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) { if(espEnabled)espLogOnce(espLoggedRenderer,bedrocktools::diagnostics::Level::Warning,"render_gate=missing_tessellator_or_mesh_function"); return; }
    if (!screenContext || reinterpret_cast<std::uintptr_t>(screenContext)<0x1000) { if(espEnabled)espLogOnce(espLoggedContext,bedrocktools::diagnostics::Level::Warning,"render_gate=screen_context_unavailable"); return; }

    uintptr_t tessellatorPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (!tessellatorPtr || tessellatorPtr < 0x1000) { if(espEnabled)espLogOnce(espLoggedRenderer,bedrocktools::diagnostics::Level::Warning,"render_gate=tessellator_unavailable"); return; }
    void* tessellator = (void*)tessellatorPtr;

    uintptr_t lrpPtr = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) { if(espEnabled)espLogOnce(espLoggedRenderer,bedrocktools::diagnostics::Level::Warning,"render_gate=level_renderer_player_unavailable"); return; }

    float camX = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    float camY = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
    float camZ = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    ensureMaterials();

    void* matInner = s_matSelection ? (void*)&s_matSelection
                                    : (void*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    uintptr_t colorHolderPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) { if(espEnabled)espLogOnce(espLoggedColor,bedrocktools::diagnostics::Level::Warning,"render_gate=color_holder_unavailable"); return; }
    float* colorHolder = (float*)colorHolderPtr;

    float savedColor[4] = { colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3] };
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    auto drawLines = [&](const std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>>& lines, uint32_t color) {
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
        s_renderMesh(screenContext, tessellator, matInner, pad);
    };

    auto drawBox = [&](const AABB& aabb, uint32_t color) {
        std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> lines;
        bedrocktools::sdk::Vec3 mn = aabb.min;
        bedrocktools::sdk::Vec3 mx = aabb.max;

        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mn.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mn.y, mn.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mn.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mn.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mn.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mn.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mn.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mn.y, mn.z}});

        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mx.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mx.y, mn.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mx.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mx.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mx.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mx.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mx.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mx.y, mn.z}});

        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mn.y, mn.z}, bedrocktools::sdk::Vec3{mn.x, mx.y, mn.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mn.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mx.y, mn.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mn.y, mx.z}, bedrocktools::sdk::Vec3{mx.x, mx.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mn.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mx.y, mx.z}});

        drawLines(lines, color);
    };

    // Camera-facing segmented neon ring, positioned in world space near the top of
    // the player's bounds. It follows the player and does not depend on Aim Assist.
    auto drawTargetRing = [&](const AABB& aabb) {
        const float width = aabb.max.x - aabb.min.x;
        const float height = aabb.max.y - aabb.min.y;
        const float centerX = (aabb.min.x + aabb.max.x) * 0.5f;
        const float centerZ = (aabb.min.z + aabb.max.z) * 0.5f;
        const bedrocktools::sdk::Vec3 center{
            centerX,
            aabb.max.y - std::max(0.06f, height * 0.18f),
            centerZ
        };

        float nx = camX - center.x;
        float ny = camY - center.y;
        float nz = camZ - center.z;
        const float nLen = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (!std::isfinite(nLen) || nLen < 0.001f) return;
        nx /= nLen; ny /= nLen; nz /= nLen;

        // right = world-up x view-normal; up = view-normal x right
        float rx = nz, ry = 0.0f, rz = -nx;
        const float rLen = std::sqrt(rx*rx + ry*ry + rz*rz);
        if (rLen < 0.001f) { rx = 1.0f; ry = 0.0f; rz = 0.0f; }
        else { rx /= rLen; ry /= rLen; rz /= rLen; }
        const float ux = ny*rz - nz*ry;
        const float uy = nz*rx - nx*rz;
        const float uz = nx*ry - ny*rx;

        const float radius = std::clamp(std::max(width * 0.95f, 0.38f), 0.34f, 0.62f);
        constexpr float tau = 6.2831853071795864769f;
        std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> ringLines;
        ringLines.reserve(112);
        auto pointAt = [&](float r, float angle) {
            return bedrocktools::sdk::Vec3{
                center.x + rx * std::cos(angle) * r + ux * std::sin(angle) * r,
                center.y + ry * std::cos(angle) * r + uy * std::sin(angle) * r,
                center.z + rz * std::cos(angle) * r + uz * std::sin(angle) * r
            };
        };

        // Outer ring has four deliberate breaks, echoing the sharp pixel-art arrow.
        constexpr int outerSegments = 64;
        for (int i = 0; i < outerSegments; ++i) {
            if ((i % 16) >= 12) continue;
            const float a0 = tau * static_cast<float>(i) / outerSegments;
            const float a1 = tau * static_cast<float>(i + 1) / outerSegments;
            ringLines.push_back({pointAt(radius, a0), pointAt(radius, a1)});
        }
        // Two clean inner rings keep the target readable without a busy center sprite.
        constexpr int middleSegments = 48;
        for (int i = 0; i < middleSegments; ++i) {
            const float a0 = tau * static_cast<float>(i) / middleSegments;
            const float a1 = tau * static_cast<float>(i + 1) / middleSegments;
            ringLines.push_back({pointAt(radius * 0.72f, a0), pointAt(radius * 0.72f, a1)});
        }
        constexpr int innerSegments = 32;
        for (int i = 0; i < innerSegments; ++i) {
            const float a0 = tau * static_cast<float>(i) / innerSegments;
            const float a1 = tau * static_cast<float>(i + 1) / innerSegments;
            ringLines.push_back({pointAt(radius * 0.42f, a0), pointAt(radius * 0.42f, a1)});
        }
        // Four short cardinal ticks give it a crisp, game-readable silhouette.
        for (int i = 0; i < 4; ++i) {
            const float a = tau * static_cast<float>(i) / 4.0f;
            ringLines.push_back({pointAt(radius * 1.06f, a), pointAt(radius * 1.24f, a)});
        }
        const std::uint32_t neonGreen = 0xFF39FF14u;
        drawLines(ringLines, neonGreen);
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
        const bool isExpandedTarget=showExpandedHitboxes&&expander&&ent!=g_localPlayerPtr&&expander->expandsType(isPlayer);
        if(wantsEsp){
            std::int32_t gameType=-1;std::uint32_t color=esp->unknownColor;
            if(bedrocktools::core::gamehooks::tryGetPlayerGameType(ent,gameType)){
                switch(gameType){case 0:color=esp->survivalColor;break;case 1:color=esp->creativeColor;break;
                 case 2:color=esp->adventureColor;break;case 3:color=esp->spectatorColor;break;default:color=esp->unknownColor;break;}
            }
            drawBox(aabb,color);
            if (esp->showLockInAim) drawTargetRing(aabb);
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
        if (!wantsEsp || isExpandedTarget) drawBox(aabb, boxColor);

        if (!nativeHitboxesEnabled) return;
        if (g_hitboxMod->showEyeLine) {
            float minX = aabb.min.x;
            float maxX = aabb.max.x;
            float minZ = aabb.min.z;
            float maxZ = aabb.max.z;

            float entityHeight = aabb.max.y - aabb.min.y;
            float eyeHeight = aabb.min.y + entityHeight * 0.85f;

            std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> eyeLines;
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

            std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> lookLines;
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

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

HitboxModule::HitboxModule()
    : Module("Hitbox", "Displays hitboxes of entities.") {

    showInMenu = true;

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
    if (tv) { m_tessVertexAddr = (void*)tv; s_tessVertex = (Tessellator_vertex_t)tv; }

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
