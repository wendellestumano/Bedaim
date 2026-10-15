#include "aimassist.hpp"
#include "AutoHitKeyBinding.hpp"
#include "AimAssistMath.hpp"
#include "AutoHitGuards.hpp"
#include "core/InventoryAccess.hpp"
#include "core/GameHooks.hpp"
#include "../ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cctype>
#include <string>
#include <limits>
#include <atomic>
#include <string_view>
#include <pl/Logger.hpp>
#include <pl/Input.hpp>
#include <pl/ModMenu.hpp>


namespace {
using ActorIsPlayerFn = bool (*)(void*);
using ActorIsInvisibleFn = bool (*)(void*);
using ActorGetNameTagFn = std::string (*)(void*);
using BlockSourceIsSolidBlockingBlockFn = bool (*)(void*, const bedrocktools::sdk::BlockPos*);
using LocalPlayerApplyTurnDeltaFn = void (*)(void*, bedrocktools::sdk::Vec2*);

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

AimAssistModule* g_aimAssist = nullptr;
std::once_flag g_autoHitInputCallbackOnce;
ActorIsPlayerFn g_actorIsPlayer = nullptr;
ActorIsInvisibleFn g_actorIsInvisible = nullptr;
ActorGetNameTagFn g_actorGetNameTag = nullptr;
LocalPlayerApplyTurnDeltaFn g_applyLocalPlayerTurnDelta = nullptr;
ActorFetchNearbyActorsSortedFn g_fetchNearbyActors = nullptr;
BlockSourceIsSolidBlockingBlockFn g_isSolidBlockingBlock = nullptr;

pl::log::Logger& autoHitLogger() {
    static auto& logger = pl::log::Logger::getOrCreate("Bedaim-AutoHit");
    return logger;
}

void logAutoHitOnce(std::atomic_bool& alreadyLogged, std::string_view message) {
    if (!alreadyLogged.exchange(true, std::memory_order_acq_rel))
        autoHitLogger().info("{}", message);
}

std::atomic_bool g_loggedAutoHitNoTarget{false};
std::atomic_bool g_loggedAutoHitStaleTarget{false};
std::atomic_bool g_loggedAutoHitNoGameMode{false};
std::atomic_bool g_loggedAutoHitOutOfRange{false};
std::atomic_bool g_loggedAutoHitBlocked{false};
std::atomic_bool g_loggedAutoHitInvalidPosition{false};
std::atomic_bool g_loggedAutoHitResult{false};
std::atomic_bool g_loggedAutoHitNoAttackRoute{false};

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;

float wrapDegrees(float angle) {
    return bedaim::aimmath::wrapDegrees(angle);
}

bool finiteVec3(const bedrocktools::sdk::Vec3& v) {
    return bedaim::aimmath::isFiniteVector({v.x, v.y, v.z});
}

bool actorIsGliding(void* actor) {
    using namespace bedrocktools::sdk;
    if (!actor) return false;

    const auto actorAddress = reinterpret_cast<std::uintptr_t>(actor);
    void* dataWrapper = reinterpret_cast<void*>(actorAddress + offsets::Actor::mEntityData);
    if (!dataWrapper) return false;

    void* component = *reinterpret_cast<void**>(dataWrapper);
    if (!component) return false;

    auto** begin = *reinterpret_cast<void***>(component);
    auto** end = *reinterpret_cast<void***>(reinterpret_cast<std::uintptr_t>(component) + sizeof(void*));
    if (!begin || !end) return false;

    const auto beginAddress = reinterpret_cast<std::uintptr_t>(begin);
    const auto endAddress = reinterpret_cast<std::uintptr_t>(end);
    if (endAddress < beginAddress ||
        (endAddress - beginAddress) % sizeof(void*) != 0 ||
        (endAddress - beginAddress) / sizeof(void*) > 192) return false;
    if (begin == end) return false;

    void* item = begin[0];
    if (!item) return false;

    const auto itemAddress = reinterpret_cast<std::uintptr_t>(item);
    const auto type = *reinterpret_cast<const std::uint8_t*>(itemAddress + offsets::DataItem::mType);
    const auto id = *reinterpret_cast<const std::uint16_t*>(itemAddress + offsets::DataItem::mId);
    if (type != 7 || id != 0) return false;

    std::uint64_t flags = 0;
    std::memcpy(&flags, reinterpret_cast<const void*>(itemAddress + offsets::DataItem::mValue), sizeof(flags));
    constexpr std::uint64_t kGlidingFlag = std::uint64_t{1} << 32;
    return (flags & kGlidingFlag) != 0;
}

bool validBounds(const bedrocktools::sdk::AABB& bounds) {
    return bedaim::aimmath::isValidBounds({
        {bounds.min.x, bounds.min.y, bounds.min.z},
        {bounds.max.x, bounds.max.y, bounds.max.z}
    });
}

float distanceSquared(const bedrocktools::sdk::Vec3& a, const bedrocktools::sdk::Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bool rayIntersectsAABB(const bedrocktools::sdk::Vec3& origin,
                       const bedrocktools::sdk::Vec3& direction,
                       const bedrocktools::sdk::AABB& bounds) {
    float tMin = 0.0f;
    float tMax = std::numeric_limits<float>::infinity();

    const auto testAxis = [&tMin, &tMax](float rayOrigin, float rayDirection,
                                         float minimum, float maximum) {
        if (std::abs(rayDirection) < 1.0e-6f) {
            return rayOrigin >= minimum && rayOrigin <= maximum;
        }

        float t1 = (minimum - rayOrigin) / rayDirection;
        float t2 = (maximum - rayOrigin) / rayDirection;
        if (t1 > t2) std::swap(t1, t2);

        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        return tMax >= tMin;
    };

    return testAxis(origin.x, direction.x, bounds.min.x, bounds.max.x) &&
           testAxis(origin.y, direction.y, bounds.min.y, bounds.max.y) &&
           testAxis(origin.z, direction.z, bounds.min.z, bounds.max.z) &&
           tMax >= 0.0f;
}

bool hasClearLineOfSight(void* blockSource,
                         const bedrocktools::sdk::Vec3& from,
                         const bedrocktools::sdk::Vec3& to) {
    if (!blockSource || !g_isSolidBlockingBlock || !finiteVec3(from) || !finiteVec3(to)) return false;

    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;
    int x = static_cast<int>(std::floor(from.x));
    int y = static_cast<int>(std::floor(from.y));
    int z = static_cast<int>(std::floor(from.z));
    const int targetX = static_cast<int>(std::floor(to.x));
    const int targetY = static_cast<int>(std::floor(to.y));
    const int targetZ = static_cast<int>(std::floor(to.z));

    const int stepX = (dx > 0.0f) - (dx < 0.0f);
    const int stepY = (dy > 0.0f) - (dy < 0.0f);
    const int stepZ = (dz > 0.0f) - (dz < 0.0f);
    const float inf = std::numeric_limits<float>::infinity();
    const float deltaX = stepX == 0 ? inf : std::abs(1.0f / dx);
    const float deltaY = stepY == 0 ? inf : std::abs(1.0f / dy);
    const float deltaZ = stepZ == 0 ? inf : std::abs(1.0f / dz);
    float maxX = stepX > 0 ? (std::floor(from.x) + 1.0f - from.x) * deltaX
                          : stepX < 0 ? (from.x - std::floor(from.x)) * deltaX : inf;
    float maxY = stepY > 0 ? (std::floor(from.y) + 1.0f - from.y) * deltaY
                          : stepY < 0 ? (from.y - std::floor(from.y)) * deltaY : inf;
    float maxZ = stepZ > 0 ? (std::floor(from.z) + 1.0f - from.z) * deltaZ
                          : stepZ < 0 ? (from.z - std::floor(from.z)) * deltaZ : inf;

    const int maxSteps = std::min(1024,
        std::abs(targetX - x) + std::abs(targetY - y) + std::abs(targetZ - z) + 4);
    for (int i = 0; i < maxSteps; ++i) {
        if (x == targetX && y == targetY && z == targetZ) return true;

        if (maxX <= maxY && maxX <= maxZ) {
            x += stepX;
            maxX += deltaX;
        } else if (maxY <= maxZ) {
            y += stepY;
            maxY += deltaY;
        } else {
            z += stepZ;
            maxZ += deltaZ;
        }

        // Do not test the target's own voxel; the endpoint may lie inside its hitbox.
        if (x == targetX && y == targetY && z == targetZ) return true;
        const bedrocktools::sdk::BlockPos cell{x, y, z};
        if (g_isSolidBlockingBlock(blockSource, &cell)) return false;
    }
    return x == targetX && y == targetY && z == targetZ;
}



bool categoryHasMobBit(void* actor) {
    if (!actor) return false;
    const auto categories = *reinterpret_cast<const std::uint32_t*>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mCategories);
    // BedrockTools' existing Hitbox module uses category bit 2 to identify entities.
    return (categories & 2u) != 0;
}

std::string normalizeName(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (i + 2 < value.size() && byte == 0xC2u &&
            static_cast<unsigned char>(value[i + 1]) == 0xA7u) {
            i += 2;
            continue;
        }
        result.push_back(static_cast<char>(std::tolower(byte)));
    }
    const auto first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = result.find_last_not_of(" \t\r\n");
    return result.substr(first, last - first + 1);
}

bool whitelistContainsName(const std::string& whitelist, const std::string& name) {
    const std::string normalizedName = normalizeName(name);
    if (normalizedName.empty()) return false;
    std::size_t start = 0;
    while (start <= whitelist.size()) {
        const std::size_t end = whitelist.find_first_of(",;\r\n", start);
        const std::string entry = normalizeName(whitelist.substr(
            start, end == std::string::npos ? std::string::npos : end - start));
        if (!entry.empty() && entry == normalizedName) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

bool isWhitelistedActor(void* actor, bool isPlayer, const std::string& whitelist) {
    if (!actor || normalizeName(whitelist).empty()) return false;
    if (isPlayer) {
        const auto* player = reinterpret_cast<bedrocktools::sdk::Player*>(actor);
        if (whitelistContainsName(whitelist, player->name())) return true;
    }
    if (g_actorGetNameTag) {
        const std::string nameTag = g_actorGetNameTag(actor);
        if (whitelistContainsName(whitelist, nameTag)) return true;
    }
    return false;
}

enum class HeldWeapon {
    None, Sword, Axe, Mace, Bow, Crossbow, Trident, Spear, OtherMelee
};

HeldWeapon heldWeaponInMainHand(const bedrocktools::sdk::Player* player) {
    if (!player) return HeldWeapon::None;
    auto** vtable = *reinterpret_cast<void***>(const_cast<bedrocktools::sdk::Player*>(player));
    constexpr std::size_t getCarriedItemIndex = bedrocktools::sdk::offsets::VTable::PlayerGetCarriedItem;
    if (!vtable || !vtable[getCarriedItemIndex]) return HeldWeapon::None;
    using GetCarriedItemFn = const void* (*)(void*);
    const void* stack = reinterpret_cast<GetCarriedItemFn>(vtable[getCarriedItemIndex])(
        const_cast<bedrocktools::sdk::Player*>(player));
    if (!stack) return HeldWeapon::None;

    // getDescriptionId/dladdr validation is expensive on Android. Resolve one
    // identifier and classify it with ordinary string comparisons instead of
    // re-entering native ItemStack inspection once for every weapon category.
    const std::string identifier = bedrocktools::core::InventoryAccess::get().stackIdentifier(stack);
    if (identifier.empty()) return HeldWeapon::None;
    if (identifier.find("crossbow") != std::string::npos) return HeldWeapon::Crossbow;
    if (identifier.find("trident") != std::string::npos) return HeldWeapon::Trident;
    if (identifier.find("mace") != std::string::npos) return HeldWeapon::Mace;
    if (identifier.find("spear") != std::string::npos) return HeldWeapon::Spear;
    if (identifier.find("sword") != std::string::npos) return HeldWeapon::Sword;
    if (identifier.find("pickaxe") == std::string::npos &&
        identifier.find("axe") != std::string::npos) return HeldWeapon::Axe;
    if (identifier.find("bow") != std::string::npos) return HeldWeapon::Bow;

    constexpr std::string_view otherWeapons[] = {
        "dagger", "katana", "rapier", "scythe", "hammer", "halberd", "glaive", "lance"
    };
    for (const auto identifierPart : otherWeapons) {
        if (identifier.find(identifierPart) != std::string::npos) return HeldWeapon::OtherMelee;
    }
    return HeldWeapon::None;
}

bool weaponAllowed(HeldWeapon weapon, bool sword, bool axe, bool mace,
                   bool bow, bool crossbow, bool trident, bool spear, bool otherMelee) {
    switch (weapon) {
        case HeldWeapon::Sword: return sword;
        case HeldWeapon::Axe: return axe;
        case HeldWeapon::Mace: return mace;
        case HeldWeapon::Bow: return bow;
        case HeldWeapon::Crossbow: return crossbow;
        case HeldWeapon::Trident: return trident;
        case HeldWeapon::Spear: return spear;
        case HeldWeapon::OtherMelee: return otherMelee;
        case HeldWeapon::None: default: return false;
    }
}

bool naturalizationAppliesToWeapon(const std::string& setting, HeldWeapon weapon) {
    if (setting == "All") return true;
    if (setting == "Sword") return weapon == HeldWeapon::Sword;
    if (setting == "Axe") return weapon == HeldWeapon::Axe;
    if (setting == "Mace") return weapon == HeldWeapon::Mace;
    if (setting == "Bow") return weapon == HeldWeapon::Bow;
    if (setting == "Crossbow") return weapon == HeldWeapon::Crossbow;
    if (setting == "Trident") return weapon == HeldWeapon::Trident;
    if (setting == "Spear") return weapon == HeldWeapon::Spear;
    if (setting == "Other melee") return weapon == HeldWeapon::OtherMelee;
    return false;
}

}

AimAssistModule::AimAssistModule()
    : Module("Aim Assist", "Smooth target tracking for private PvP training worlds.") {
    keybind = 0;
    g_aimAssist = this;
}

AimAssistModule::~AimAssistModule() {
    if (g_aimAssist == this) g_aimAssist = nullptr;
}

void AimAssistModule::resetTrackingState() {
    m_lastPlayer = nullptr;
    m_lastTarget = nullptr;
    m_targetPresent = false;
    m_lastTargetIsPlayer = false;
    m_cachedHeldWeaponCode = 0;
    m_cachedWeaponSlot = -1;
    m_nextHeldWeaponRefresh = {};
    m_weaponFilterAllowsCurrent = true;
    m_lastTargetBounds = {};
    m_lastAimPoint = {};
    m_hasAimPoint = false;
    m_localPlayerGliding = false;
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    m_pitchHoldUntil = {};
    m_lastObservedRotation = {};
    m_hasObservedRotation = false;
    m_lastAssistYawDelta = 0.0f;
    m_lastAssistPitchDelta = 0.0f;
    m_lastRotationUpdate = {};
    m_lastTargetSeen = {};
    m_motionTrackedTarget = nullptr;
    m_lastMotionPosition = {};
    m_targetVelocity = {};
    m_hasMotionSample = false;
    m_lastMotionSample = {};
    m_lastPredictionPlayerPosition = {};
    m_playerVelocity = {};
    m_hasPredictionPlayerSample = false;
    m_lastPredictionPlayerSample = {};
}

void AimAssistModule::onInit() {
    // The launcher Input API observes real/virtual Android key events. It does
    // not synthesize them from native code; the floating Hold button below is
    // the supported way to emit a configurable key into the game's input path.
    std::call_once(g_autoHitInputCallbackOnce, [] {
        pl::input::registerKeyCallback([](const pl::input::KeyEvent& event) {
            auto* module = g_aimAssist;
            if (!module) return false;
            const int configuredKey = module->m_autoHitKeyCode.load(std::memory_order_acquire);
            if (configuredKey > 0 && event.keyCode == configuredKey) {
                module->m_autoHitPhysicalKeyDown.store(event.isKeyDown, std::memory_order_release);
            }
            return false;
        });
    });

    g_fetchNearbyActors = reinterpret_cast<ActorFetchNearbyActorsSortedFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));
    g_isSolidBlockingBlock = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock));
    g_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    g_actorIsInvisible = reinterpret_cast<ActorIsInvisibleFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsInvisible));
    g_actorGetNameTag = reinterpret_cast<ActorGetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));
    g_applyLocalPlayerTurnDelta = reinterpret_cast<LocalPlayerApplyTurnDeltaFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LocalPlayerApplyTurnDelta));

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_aimAssist && g_aimAssist->enabled) {
                g_aimAssist->processPlayerTick(event.player);
            }
        });
    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>(
        [](auto& event) {
            auto* module = g_aimAssist;
            if (!module) return;
            if (!event.clientInstance) return;
            // Reuse the known-good player pointer from LocalPlayerTickEvent.
            // A second localPlayer() lookup may transiently return null and wipe tracking.
            module->processPlayerFrame();
        });


}

void AimAssistModule::onEnable() {
    m_autoHitPhysicalKeyDown.store(false, std::memory_order_release);
    m_autoHitVirtualKeyDown.store(false, std::memory_order_release);
    m_autoHitMouseButtonDown.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    resetTrackingState();
}

void AimAssistModule::onDisable() {
    m_autoHitPhysicalKeyDown.store(false, std::memory_order_release);
    m_autoHitVirtualKeyDown.store(false, std::memory_order_release);
    m_autoHitMouseButtonDown.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    resetTrackingState();
}

bool AimAssistModule::showInLegacyMenu(std::string_view key) const {
    return key != "m_autoHitKeyCode";
}

bool AimAssistModule::onMouseEvent(int button, bool isDown) {
    if (!enabled) return false;
    const int configuredButton = m_autoHitMouseButton.load(std::memory_order_acquire);
    if (configuredButton > 0 && button == configuredButton) {
        m_autoHitMouseButtonDown.store(isDown, std::memory_order_release);
    }
    // Never consume the user's mouse event; AutoHit observes it passively.
    return false;
}

void AimAssistModule::onMenuRegistered() {
    m_menuRegistered.store(true, std::memory_order_release);
    refreshAutoHitKeyButton();
}

void AimAssistModule::refreshAutoHitKeyButton() {
    using namespace pl::modmenu;
    constexpr std::string_view ButtonId = "bedaim.aimassist.autohit_key";
    unregisterButton(ButtonId);

    const int keyCode = m_autoHitKeyCode.load(std::memory_order_acquire);
    const int mouseButton = m_autoHitMouseButton.load(std::memory_order_acquire);
    if ((keyCode <= 0 || keyCode > 288) && (mouseButton <= 0 || mouseButton > 16)) return;

    std::string label;
    if (mouseButton > 0) {
        label = "AUTO HIT / BUTTON " + std::to_string(mouseButton);
    } else {
        std::string keyLabel = bedaim::autohitkey::format(keyCode);
        std::transform(keyLabel.begin(), keyLabel.end(), keyLabel.begin(),
            [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        if (keyCode >= 7 && keyCode <= 16) {
            keyLabel = std::to_string(keyCode == 7 ? 0 : keyCode - 6);
        } else if (keyCode >= 29 && keyCode <= 54) {
            keyLabel.assign(1, static_cast<char>('A' + (keyCode - 29)));
        } else if (keyCode == 62) {
            keyLabel = "SPACE";
        } else if (keyCode == 66) {
            keyLabel = "ENTER";
        } else if (keyCode == 111) {
            keyLabel = "ESC";
        }
        label = "AUTO HIT / KEY " + keyLabel;
    }

    ButtonBuilder button(std::string(ButtonId), "AutoHit Key");
    button.moduleId(moduleId)
        .modId("BedrockTools")
        .label(label)
        .behavior(ButtonBehavior::Hold)
        .defaultVisible(true)
        .stylePreset(ButtonStylePreset::Keycap);
    // Mouse aliases are represented by this same touch Hold control on Android;
    // do not inject an unrelated Android key when the user selected button1.
    if (keyCode > 0) button.androidKeyCode(keyCode);
    button.onEvent([this, mouseButton](std::string_view, ButtonEvent event, float) {
        const bool down = event == ButtonEvent::Down;
        if (mouseButton > 0) {
            m_autoHitMouseButtonDown.store(down, std::memory_order_release);
        } else {
            m_autoHitVirtualKeyDown.store(down, std::memory_order_release);
        }
    }).registerButton();
}






void AimAssistModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    processPlayerTickLocked(player);
}

bool AimAssistModule::currentTargetSnapshot(void*& target, bedrocktools::sdk::AABB& bounds, float& range) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    target = nullptr;
    bounds = {};
    range = std::clamp(m_range, 2.0f, 80.0f);
    if (!enabled || !m_lastPlayer || !m_lastTarget || !m_targetPresent || !validBounds(m_lastTargetBounds)) return false;
    target = m_lastTarget;
    bounds = m_lastTargetBounds;
    return true;
}

bool AimAssistModule::targetBoundsSnapshot(void* desiredTarget,
                                                   bedrocktools::sdk::AABB& bounds,
                                                   float& range) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    bounds = {};
    range = std::clamp(m_range, 2.0f, 80.0f);
    auto* player = m_lastPlayer;
    if (!enabled || !player || !desiredTarget || !g_fetchNearbyActors) return false;

    // Resolve the desired actor against a fresh engine-owned actor snapshot before
    // dereferencing it. This lets short multi-tick actions (such as a mace swap)
    // keep their original target without trusting a cached raw pointer.
    Vec3 extent{range, range, range};
    ActorVector actors = g_fetchNearbyActors(player, &extent, 1);
    const auto beginAddress = reinterpret_cast<std::uintptr_t>(actors.begin);
    const auto endAddress = reinterpret_cast<std::uintptr_t>(actors.end);
    if (!actors.begin || !actors.end || endAddress < beginAddress ||
        (endAddress - beginAddress) % sizeof(DistanceSortedActor) != 0 ||
        (endAddress - beginAddress) / sizeof(DistanceSortedActor) > 4096) return false;

    for (auto* entry = actors.begin; entry < actors.end; ++entry) {
        if (entry->actor != desiredTarget) continue;
        auto* actor = reinterpret_cast<Actor*>(desiredTarget);
        const Vec3 position = actor->position();
        const AABB candidateBounds = actor->bounds();
        if (!finiteVec3(position) || !validBounds(candidateBounds)) return false;
        bounds = candidateBounds;
        return true;
    }
    return false;
}

bool AimAssistModule::currentTargetInCrosshair(bedrocktools::sdk::Player* player, void* target, float tolerance) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    if (!enabled || !player || player != m_lastPlayer || !target ||
        target != m_lastTarget || !m_targetPresent || !validBounds(m_lastTargetBounds)) return false;

    auto* rotationComponent = player->rotationComponent();
    if (!rotationComponent) return false;
    Vec2 rotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y)) return false;
    rotation.x = std::clamp(rotation.x, -89.0f, 89.0f);
    rotation.y = wrapDegrees(rotation.y);

    const Vec3 playerPosition = player->position();
    const AABB playerBounds = player->bounds();
    if (!finiteVec3(playerPosition)) return false;
    Vec3 eyePosition = playerPosition;
    if (validBounds(playerBounds)) {
        eyePosition.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        eyePosition.y = playerBounds.min.y + (playerBounds.max.y - playerBounds.min.y) * 0.85f;
        eyePosition.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }

    const float yaw = rotation.y / kRadToDeg;
    const float pitch = rotation.x / kRadToDeg;
    const float cosPitch = std::cos(pitch);
    const Vec3 viewDirection{
        -std::sin(yaw) * cosPitch,
        -std::sin(pitch),
        std::cos(yaw) * cosPitch
    };
    // Small tolerance handles mobile touch quantization. When enabled, the
    // dedicated expander participates in this exact same target-ray test.
    const float crosshairTolerance = std::clamp(tolerance, 0.05f, 0.8f);
    AABB targetQueryBounds = m_lastTargetBounds;
    auto* expanderBase = ModuleRegistry::get().find("bedrocktools.Hitbox Expander");
    auto* expander = (expanderBase && expanderBase->enabled)
        ? static_cast<HitboxExpanderModule*>(expanderBase) : nullptr;
    if (expander && g_actorIsPlayer) {
        targetQueryBounds = expander->expandedTargetBounds(g_actorIsPlayer(target), m_lastTargetBounds);
    }
    const AABB crosshairBounds{
        {targetQueryBounds.min.x - crosshairTolerance,
         targetQueryBounds.min.y - crosshairTolerance,
         targetQueryBounds.min.z - crosshairTolerance},
        {targetQueryBounds.max.x + crosshairTolerance,
         targetQueryBounds.max.y + crosshairTolerance,
         targetQueryBounds.max.z + crosshairTolerance}
    };
    if (!rayIntersectsAABB(eyePosition, viewDirection, crosshairBounds)) return false;

    if (m_cantSeeThroughBlocks || (expander && expander->requiresLineOfSight())) {
        auto* dimension = player->dimension();
        void* blockSource = dimension ? static_cast<void*>(dimension->blockSource()) : nullptr;
        const Vec3 center{
            (m_lastTargetBounds.min.x + m_lastTargetBounds.max.x) * 0.5f,
            (m_lastTargetBounds.min.y + m_lastTargetBounds.max.y) * 0.5f,
            (m_lastTargetBounds.min.z + m_lastTargetBounds.max.z) * 0.5f
        };
        if (blockSource && g_isSolidBlockingBlock &&
            !hasClearLineOfSight(blockSource, eyePosition, center)) return false;
    }
    return true;
}

void AimAssistModule::deferAutoHitFor(float delayMs) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    const auto requested = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(static_cast<int>(std::clamp(delayMs, 80.0f, 1000.0f)));
    if (m_nextAutoHit < requested) m_nextAutoHit = requested;
}

void AimAssistModule::processPlayerTickLocked(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;

    if (!enabled || !player || !g_fetchNearbyActors || (!m_targetPlayers && !m_targetMobs)) {
        resetTrackingState();
        return;
    }

    m_lastPlayer = player;
    m_localPlayerGliding = actorIsGliding(player);

    // Native ItemStack description lookup is relatively expensive. Reclassify
    // immediately on hotbar changes and otherwise at 10 Hz; weapon toggles still
    // apply every tick to the cached category, without repeating native lookups.
    auto& inventory = bedrocktools::core::InventoryAccess::get();
    const int selectedWeaponSlot = inventory.selectedHotbarSlot(player);
    const auto weaponCheckTime = std::chrono::steady_clock::now();
    if (selectedWeaponSlot != m_cachedWeaponSlot ||
        m_nextHeldWeaponRefresh.time_since_epoch().count() == 0 ||
        weaponCheckTime >= m_nextHeldWeaponRefresh) {
        m_cachedHeldWeaponCode = static_cast<int>(heldWeaponInMainHand(player));
        m_cachedWeaponSlot = selectedWeaponSlot;
        m_nextHeldWeaponRefresh = weaponCheckTime + std::chrono::milliseconds(100);
    }
    const HeldWeapon heldWeapon = static_cast<HeldWeapon>(m_cachedHeldWeaponCode);
    m_weaponFilterAllowsCurrent = !m_weaponOnly ||
        weaponAllowed(heldWeapon, m_weaponSword, m_weaponAxe, m_weaponMace,
            m_weaponBow, m_weaponCrossbow, m_weaponTrident, m_weaponSpear, m_weaponOtherMelee);
    // Keep target geometry fresh for Sword/Mace Swapper even when the currently
    // held weapon is filtered out. Camera movement and AutoHit still respect the filter.

    auto* rotationComponent = player->rotationComponent();
    if (!rotationComponent) {
        resetTrackingState();
        return;
    }

    auto* expanderBase = ModuleRegistry::get().find("bedrocktools.Hitbox Expander");
    auto* expander = (expanderBase && expanderBase->enabled)
        ? static_cast<HitboxExpanderModule*>(expanderBase) : nullptr;
    const bool expanderRequiresLineOfSight = expander && expander->requiresLineOfSight();

    void* blockSource = nullptr;
    if (m_cantSeeThroughBlocks || expanderRequiresLineOfSight) {
        auto* dimension = player->dimension();
        blockSource = dimension ? static_cast<void*>(dimension->blockSource()) : nullptr;
    }

    Vec2 currentRotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(currentRotation.x) || !std::isfinite(currentRotation.y)) {
        resetTrackingState();
        return;
    }
    currentRotation.x = std::clamp(currentRotation.x, -89.0f, 89.0f);
    currentRotation.y = wrapDegrees(currentRotation.y);

    const AABB playerBounds = player->bounds();
    const Vec3 playerPosition = player->position();
    if (!finiteVec3(playerPosition)) {
        resetTrackingState();
        return;
    }

    // Detect the start of an upward jump from consecutive local-player positions.
    // Hold pitch steady briefly so the assist can keep tracking horizontally
    // without pulling the camera toward the target's lower/central hitbox mid-jump.
    const bool naturalizationForWeapon =
        m_naturalization && naturalizationAppliesToWeapon(m_naturalizationWeapon, heldWeapon);
    if (naturalizationForWeapon) {
        if (m_hasLastPlayerY) {
            const float verticalDelta = playerPosition.y - m_lastPlayerY;
            if (std::isfinite(verticalDelta) && verticalDelta > 0.035f &&
                verticalDelta < 0.85f) {
                m_pitchHoldUntil = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(550);
            }
        }
        m_lastPlayerY = playerPosition.y;
        m_hasLastPlayerY = true;
    } else {
        m_lastPlayerY = playerPosition.y;
        m_hasLastPlayerY = true;
        m_pitchHoldUntil = {};
    }

    Vec3 eyePosition = playerPosition;
    if (validBounds(playerBounds)) {
        eyePosition.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        eyePosition.y = playerBounds.min.y + (playerBounds.max.y - playerBounds.min.y) * 0.85f;
        eyePosition.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }

    const float range = std::clamp(m_range, 2.0f, 80.0f);
    const float expansionMargin = expander ? expander->queryExpansionMargin() : 0.0f;
    const float scanRange = bedaim::autohit::effectiveScanRange(range, m_autoHit, m_autoHitRange, expansionMargin);
    Vec3 extent{scanRange, scanRange, scanRange};
    // The native nearby-actors function returns the actual candidate snapshot;
    // don't gate it on Level::mActorManager, an unused offset that can vary by build.
    ActorVector actors = g_fetchNearbyActors(player, &extent, 1);
    const auto actorsBeginAddress = reinterpret_cast<std::uintptr_t>(actors.begin);
    const auto actorsEndAddress = reinterpret_cast<std::uintptr_t>(actors.end);
    if (!actors.begin || !actors.end || actorsEndAddress < actorsBeginAddress ||
        (actorsEndAddress - actorsBeginAddress) % sizeof(DistanceSortedActor) != 0 ||
        (actorsEndAddress - actorsBeginAddress) / sizeof(DistanceSortedActor) > 4096) {
        // Reject malformed native vector metadata before iterating it.
        resetTrackingState();
        return;
    }

    void* bestTarget = nullptr;
    AABB bestTargetBounds{};
    bool bestTargetIsPlayer = false;
    float bestScore = std::numeric_limits<float>::max();
    void* retainedTarget = nullptr;
    AABB retainedTargetBounds{};
    float retainedTargetScore = std::numeric_limits<float>::max();
    bool retainedTargetInsideBubble = false;
    bool retainedTargetIsPlayer = false;
    Vec3 playerCenter = playerPosition;
    if (validBounds(playerBounds)) {
        playerCenter.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        playerCenter.y = (playerBounds.min.y + playerBounds.max.y) * 0.5f;
        playerCenter.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    }
    const float retentionRadius = m_localPlayerGliding
        ? 0.0f
        : std::clamp(m_targetRetentionRadius, 0.5f, 5.0f);
    const float retentionRadiusSq = retentionRadius * retentionRadius;
    const float maxDistanceSq = range * range;
    const float autoHitRange = std::clamp(m_autoHitRange, 2.0f, 6.0f);
    const float maxAutoHitDistanceSq = autoHitRange * autoHitRange;
    // Experimental full-range FOV. Range, entity type, visibility, and valid bounds
    // remain required even when the angular limit is set to 360 degrees.
    const float maxFov = std::clamp(m_fov, 1.0f, 360.0f);
    const float yawRadians = currentRotation.y / kRadToDeg;
    const float pitchRadians = currentRotation.x / kRadToDeg;
    const float cosPitch = std::cos(pitchRadians);
    const Vec3 viewDirection{
        -std::sin(yawRadians) * cosPitch,
        -std::sin(pitchRadians),
        std::cos(yawRadians) * cosPitch
    };

    for (auto* entry = actors.begin; entry < actors.end; ++entry) {
        void* candidate = entry->actor;
        if (!candidate || candidate == player) continue;

        const bool isPlayer = g_actorIsPlayer && g_actorIsPlayer(candidate);
        if (isPlayer) {
            if (!m_targetPlayers) continue;
        } else {
            if (!m_targetMobs || !categoryHasMobBit(candidate)) continue;
        }

        if (isWhitelistedActor(candidate, isPlayer, m_whitelistNames)) continue;
        if (g_actorIsInvisible && g_actorIsInvisible(candidate)) continue;

        auto* target = reinterpret_cast<Actor*>(candidate);
        const Vec3 targetPosition = target->position();
        const AABB targetBounds = target->bounds();
        if (!finiteVec3(targetPosition) || !validBounds(targetBounds)) continue;

        // Aim at a stable point on the native bounds but allow the bounded
        // expanded client box to participate in target acquisition and AutoHit range.
        const AABB selectionBounds = expander
            ? expander->expandedTargetBounds(isPlayer, targetBounds) : targetBounds;
        const Vec3 centerAimPoint{
            (targetBounds.min.x + targetBounds.max.x) * 0.5f,
            targetBounds.min.y + (targetBounds.max.y - targetBounds.min.y) * 0.55f,
            (targetBounds.min.z + targetBounds.max.z) * 0.5f
        };
        const Vec3 nearestOnSelection{
            std::clamp(eyePosition.x, selectionBounds.min.x, selectionBounds.max.x),
            std::clamp(eyePosition.y, selectionBounds.min.y, selectionBounds.max.y),
            std::clamp(eyePosition.z, selectionBounds.min.z, selectionBounds.max.z)
        };
        const float selectionDistanceSq = distanceSquared(eyePosition, nearestOnSelection);

        // Fail closed when the visibility signature or block source is unavailable.
        // hasClearLineOfSight() returns false if it cannot safely perform the check.
        // If the LOS signature is unavailable for this Minecraft build, don't
        // silently discard every candidate. Enforce LOS only when the checker is live.
        if ((m_cantSeeThroughBlocks || expanderRequiresLineOfSight) && blockSource &&
            g_isSolidBlockingBlock && !hasClearLineOfSight(blockSource, eyePosition, centerAimPoint)) continue;

        const float d2 = distanceSquared(eyePosition, centerAimPoint);
        // Range checks use the nearest point on the selected hitbox geometry, not
        // its center; the box itself still cannot exceed the configured size limits.
        const bool inAimAssistRange = std::isfinite(selectionDistanceSq) &&
            selectionDistanceSq <= maxDistanceSq;
        const bool inAutoHitRange = m_autoHit && std::isfinite(selectionDistanceSq) &&
            selectionDistanceSq <= maxAutoHitDistanceSq;
        if (!std::isfinite(d2) || d2 < 0.01f || (!inAimAssistRange && !inAutoHitRange)) continue;

        // If the current target is right beside/around the player, keep it locked
        // while it crosses behind the camera. Measure distance to the target AABB,
        // not its center, so large hitboxes don't cause a sudden lock drop.
        bool insideRetentionBubble = false;
        if (candidate == m_lastTarget) {
            const Vec3 nearestOnTarget{
                std::clamp(playerCenter.x, targetBounds.min.x, targetBounds.max.x),
                std::clamp(playerCenter.y, targetBounds.min.y, targetBounds.max.y),
                std::clamp(playerCenter.z, targetBounds.min.z, targetBounds.max.z)
            };
            insideRetentionBubble = !m_localPlayerGliding &&
                distanceSquared(playerCenter, nearestOnTarget) <= retentionRadiusSq;
        }

        const float dx = centerAimPoint.x - eyePosition.x;
        const float dy = centerAimPoint.y - eyePosition.y;
        const float dz = centerAimPoint.z - eyePosition.z;
        const float horizontal = std::sqrt(dx * dx + dz * dz);
        const float desiredYaw = std::atan2(-dx, dz) * kRadToDeg;
        const float desiredPitch = -std::atan2(dy, std::max(horizontal, 0.001f)) * kRadToDeg;

        const float targetDistance = std::sqrt(d2);
        if (!std::isfinite(targetDistance) || targetDistance <= 0.001f) continue;

        const float yawError = wrapDegrees(desiredYaw - currentRotation.y);
        const float pitchError = desiredPitch - currentRotation.x;
        const float angleScore = std::sqrt(yawError * yawError + pitchError * pitchError);
        if (!std::isfinite(angleScore)) continue;

        // Keep targets directly below the player's feet eligible during a mace
        // slam, and permit a precise ray hit through the configured expanded box.
        const float playerFeetY = validBounds(playerBounds) ? playerBounds.min.y : playerPosition.y;
        const bool candidateBelowPlayer = !m_localPlayerGliding &&
            targetBounds.max.y <= playerFeetY + 0.12f;
        const bool rayHitsExpandedTarget = expander &&
            rayIntersectsAABB(eyePosition, viewDirection, selectionBounds);

        // The last target also gets a small 3D retention bubble, including when it
        // passes below the player or behind the camera. This prevents target loss
        // during the fast vertical movement of a slam.
        if (angleScore > maxFov && !insideRetentionBubble && !candidateBelowPlayer &&
            !rayHitsExpandedTarget) continue;

        // A ray through the expanded box is a high-confidence crosshair match,
        // not an angle-only candidate that happens to fall inside the FOV.
        const bool playerDistanceMode = (m_targetSelectionMode == "Player");
        const float selectionAngleScore = rayHitsExpandedTarget ? 0.0f : angleScore;
        const float score = playerDistanceMode
            ? targetDistance
            : selectionAngleScore + targetDistance * 0.0001f;
        if (candidate == m_lastTarget) {
            retainedTarget = candidate;
            retainedTargetBounds = targetBounds;
            retainedTargetIsPlayer = isPlayer;
            retainedTargetScore = score;
            retainedTargetInsideBubble = insideRetentionBubble || candidateBelowPlayer;
        }
        if (score < bestScore) {
            bestScore = score;
            bestTarget = candidate;
            bestTargetBounds = targetBounds;
            bestTargetIsPlayer = isPlayer;
        }
    }

    // Prefer the current target when a challenger is only marginally better.
    // Angular mode uses degrees; player-distance mode uses blocks, so their
    // margins stay independent and physically meaningful.
    const bool playerDistanceMode = (m_targetSelectionMode == "Player");
    const float switchMargin = playerDistanceMode
        ? std::clamp(m_targetSwitchDistanceMargin, 0.0f, 5.0f)
        : std::clamp(m_targetSwitchHysteresis, 0.0f, 12.0f);
    if (retainedTarget && (retainedTargetInsideBubble || retainedTargetScore <= bestScore + switchMargin)) {
        bestTarget = retainedTarget;
        bestTargetBounds = retainedTargetBounds;
        bestTargetIsPlayer = retainedTargetIsPlayer;
    }

    if (!bestTarget) {
        // Keep the last target briefly while it crosses the player's immediate
        // area or the native nearby-actor list skips one tick. Do not dereference
        // the cached entity or auto-hit it while it is missing from the list.
        const auto now = std::chrono::steady_clock::now();
        if (m_lastTarget && validBounds(m_lastTargetBounds) &&
            m_lastTargetSeen.time_since_epoch().count() != 0 &&
            now - m_lastTargetSeen <= std::chrono::milliseconds(250)) {
            const Vec3 nearestOnPreviousTarget{
                std::clamp(playerCenter.x, m_lastTargetBounds.min.x, m_lastTargetBounds.max.x),
                std::clamp(playerCenter.y, m_lastTargetBounds.min.y, m_lastTargetBounds.max.y),
                std::clamp(playerCenter.z, m_lastTargetBounds.min.z, m_lastTargetBounds.max.z)
            };
            if (distanceSquared(playerCenter, nearestOnPreviousTarget) <= retentionRadiusSq) {
                m_targetPresent = false;
                return;
            }
        }
        resetTrackingState();
        return;
    }

    const bool targetChanged = bestTarget != m_lastTarget;
    const auto motionSampleTime = std::chrono::steady_clock::now();

    // Sample our own motion as well as the target. The prediction code below
    // subtracts local-player velocity from target velocity; without this sample
    // the local term stayed zero forever, so moving toward a stationary target
    // never influenced the predicted relative aim point.
    if (m_hasPredictionPlayerSample && finiteVec3(playerPosition)) {
        const float playerDt = std::chrono::duration<float>(
            motionSampleTime - m_lastPredictionPlayerSample).count();
        if (std::isfinite(playerDt) && playerDt >= 0.01f && playerDt <= 0.25f) {
            const Vec3 measuredVelocity{
                (playerPosition.x - m_lastPredictionPlayerPosition.x) / playerDt,
                (playerPosition.y - m_lastPredictionPlayerPosition.y) / playerDt,
                (playerPosition.z - m_lastPredictionPlayerPosition.z) / playerDt
            };
            const float playerSpeedSq = distanceSquared(measuredVelocity, Vec3{});
            if (std::isfinite(playerSpeedSq) && playerSpeedSq <= 225.0f) {
                m_playerVelocity.x = m_playerVelocity.x * 0.35f + measuredVelocity.x * 0.65f;
                m_playerVelocity.y = m_playerVelocity.y * 0.35f + measuredVelocity.y * 0.65f;
                m_playerVelocity.z = m_playerVelocity.z * 0.35f + measuredVelocity.z * 0.65f;
            } else {
                m_playerVelocity = {};
            }
        } else {
            m_playerVelocity = {};
        }
    } else {
        m_playerVelocity = {};
    }
    m_lastPredictionPlayerPosition = finiteVec3(playerPosition) ? playerPosition : Vec3{};
    m_hasPredictionPlayerSample = finiteVec3(playerPosition);
    m_lastPredictionPlayerSample = motionSampleTime;

    const Vec3 targetPosition = reinterpret_cast<Actor*>(bestTarget)->position();
    if (targetChanged || m_motionTrackedTarget != bestTarget || !m_hasMotionSample ||
        !finiteVec3(targetPosition)) {
        m_targetVelocity = {};
    } else {
        const float dt = std::chrono::duration<float>(motionSampleTime - m_lastMotionSample).count();
        if (std::isfinite(dt) && dt >= 0.01f && dt <= 0.25f) {
            Vec3 velocity{
                (targetPosition.x - m_lastMotionPosition.x) / dt,
                (targetPosition.y - m_lastMotionPosition.y) / dt,
                (targetPosition.z - m_lastMotionPosition.z) / dt
            };
            const float speedSq = distanceSquared(velocity, Vec3{});
            if (std::isfinite(speedSq) && speedSq <= 225.0f) {
                m_targetVelocity.x = m_targetVelocity.x * 0.35f + velocity.x * 0.65f;
                m_targetVelocity.y = m_targetVelocity.y * 0.35f + velocity.y * 0.65f;
                m_targetVelocity.z = m_targetVelocity.z * 0.35f + velocity.z * 0.65f;
            } else {
                m_targetVelocity = {};
            }
        } else {
            m_targetVelocity = {};
        }
    }
    m_motionTrackedTarget = bestTarget;
    m_lastMotionPosition = finiteVec3(targetPosition) ? targetPosition : Vec3{};
    m_hasMotionSample = finiteVec3(targetPosition);
    m_lastMotionSample = motionSampleTime;
    if (targetChanged) {
        m_hasAimPoint = false;
        m_lastRotationUpdate = {};
    }
    m_lastTarget = bestTarget;
    m_lastTargetIsPlayer = bestTargetIsPlayer;
    m_targetPresent = true;
    m_lastTargetBounds = bestTargetBounds;
    m_lastTargetSeen = motionSampleTime;
    // Also check Auto Hit here so it is independent of the client-frame render hook.
    processAutoHitLocked(player);
}


void AimAssistModule::processPlayerFrame() {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    auto* player = m_lastPlayer;
    if (!enabled || !player || !m_lastTarget || !m_targetPresent ||
        !m_weaponFilterAllowsCurrent || (!g_applyLocalPlayerTurnDelta && !m_autoHit)) return;

    // Candidate scanning is performed once per player tick. Re-running the native
    // nearby-actor query on every render frame caused duplicate 80-block scans and
    // unnecessary native string/geometry work on low-end Android.
    const auto now = std::chrono::steady_clock::now();
    if (m_lastTargetSeen.time_since_epoch().count() == 0 ||
        now < m_lastTargetSeen ||
        now - m_lastTargetSeen > std::chrono::milliseconds(250) ||
        !validBounds(m_lastTargetBounds)) {
        return;
    }

    applyAimStepLocked(player);
}

void AimAssistModule::processAutoHitLocked(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;

    // Attack only the target chosen from this tick's native actor snapshot. Do not
    // require a second camera-angle/crosshair test here: that duplicate gate caused
    // AutoHit to stop during camera convergence and when targets moved below the player.
    if (!m_autoHit || !enabled || !player || player != m_lastPlayer ||
        !m_weaponFilterAllowsCurrent) return;
    const int configuredKey = m_autoHitKeyCode.load(std::memory_order_acquire);
    const int configuredMouseButton = m_autoHitMouseButton.load(std::memory_order_acquire);
    if (configuredKey > 0 &&
        !m_autoHitPhysicalKeyDown.load(std::memory_order_acquire) &&
        !m_autoHitVirtualKeyDown.load(std::memory_order_acquire)) {
        return;
    }
    if (configuredMouseButton > 0 &&
        !m_autoHitMouseButtonDown.load(std::memory_order_acquire)) {
        return;
    }
    if (!m_lastTarget || !m_targetPresent || !validBounds(m_lastTargetBounds)) {
        logAutoHitOnce(g_loggedAutoHitNoTarget,
            "AutoHit is enabled but Aim Assist has no valid target snapshot. Check target filters and detection range.");
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_lastTargetSeen.time_since_epoch().count() == 0 ||
        now - m_lastTargetSeen > std::chrono::milliseconds(150)) {
        logAutoHitOnce(g_loggedAutoHitStaleTarget,
            "AutoHit skipped an expired target snapshot (>150 ms old).");
        return;
    }
    if (m_nextAutoHit.time_since_epoch().count() != 0 && now < m_nextAutoHit) return;

    // processAutoHitLocked is called synchronously at the end of the same
    // LocalPlayerTickEvent that selected m_lastTarget from a validated native actor
    // vector. Re-querying/sorting an 80-block actor cube here duplicates the most
    // expensive native scan every tick and was a major mobile frame-time cost.
    // Use the already validated current-tick target and its type/bounds snapshot.
    auto* expanderBase = ModuleRegistry::get().find("bedrocktools.Hitbox Expander");
    auto* expander = (expanderBase && expanderBase->enabled)
        ? static_cast<HitboxExpanderModule*>(expanderBase) : nullptr;
    auto* liveTarget = reinterpret_cast<Actor*>(m_lastTarget);
    const Vec3 liveTargetPosition = liveTarget->position();
    const AABB liveTargetBounds = liveTarget->bounds();
    if (!finiteVec3(liveTargetPosition) || !validBounds(liveTargetBounds)) {
        logAutoHitOnce(g_loggedAutoHitInvalidPosition,
            "AutoHit skipped a tick-selected target with invalid position or bounds.");
        m_nextAutoHit = now + std::chrono::milliseconds(100);
        return;
    }
    m_lastTargetBounds = liveTargetBounds;
    const bool liveTargetIsPlayer = m_lastTargetIsPlayer;

    const Vec3 playerPosition = player->position();
    const AABB playerBounds = player->bounds();
    if (!finiteVec3(playerPosition)) {
        logAutoHitOnce(g_loggedAutoHitInvalidPosition,
            "AutoHit skipped a non-finite local-player position.");
        return;
    }

    Vec3 eyePosition = playerPosition;
    if (validBounds(playerBounds)) {
        eyePosition.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        eyePosition.y = playerBounds.min.y + (playerBounds.max.y - playerBounds.min.y) * 0.85f;
        eyePosition.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }

    AABB bounds = liveTargetBounds;
    if (expander && g_actorIsPlayer) {
        bounds = expander->expandedTargetBounds(liveTargetIsPlayer, bounds);
    }
    const Vec3 targetCenter{
        (bounds.min.x + bounds.max.x) * 0.5f,
        bounds.min.y + (bounds.max.y - bounds.min.y) * 0.55f,
        (bounds.min.z + bounds.max.z) * 0.5f
    };
    const float centerDistanceSq = distanceSquared(eyePosition, targetCenter);
    if (!std::isfinite(centerDistanceSq) || centerDistanceSq < 0.01f) {
        logAutoHitOnce(g_loggedAutoHitInvalidPosition,
            "AutoHit skipped invalid/overlapping eye and target coordinates.");
        return;
    }

    // Choose the nearest point on the target bounds. If the player's eye happens to
    // overlap the AABB, use the target center rather than producing a zero-length hit.
    Vec3 hitPoint{
        std::clamp(eyePosition.x, bounds.min.x, bounds.max.x),
        std::clamp(eyePosition.y, bounds.min.y, bounds.max.y),
        std::clamp(eyePosition.z, bounds.min.z, bounds.max.z)
    };
    float hitDistanceSq = distanceSquared(eyePosition, hitPoint);
    if (!std::isfinite(hitDistanceSq)) {
        logAutoHitOnce(g_loggedAutoHitInvalidPosition,
            "AutoHit skipped non-finite hit-point coordinates.");
        return;
    }
    if (hitDistanceSq < 0.01f) {
        hitPoint = targetCenter;
        hitDistanceSq = centerDistanceSq;
    }

    const float autoHitRange = std::clamp(m_autoHitRange, 2.0f, 6.0f);
    if (hitDistanceSq > autoHitRange * autoHitRange) {
        logAutoHitOnce(g_loggedAutoHitOutOfRange,
            "AutoHit found a target, but its hitbox is beyond the configured AutoHit range.");
        return;
    }

    if (m_cantSeeThroughBlocks || (expander && expander->requiresLineOfSight())) {
        auto* dimension = player->dimension();
        void* blockSource = dimension ? static_cast<void*>(dimension->blockSource()) : nullptr;
        if (blockSource && g_isSolidBlockingBlock &&
            !hasClearLineOfSight(blockSource, eyePosition, hitPoint)) {
            logAutoHitOnce(g_loggedAutoHitBlocked,
                "AutoHit is blocked by the line-of-sight setting.");
            return;
        }
    }

    // Resolve the native receiver only after cheap geometry and visibility gates
    // pass. When no engine-owned GameMode has been observed yet, throttle this
    // lookup instead of checking mappings on every player tick.
    void* gameMode = bedrocktools::core::gamehooks::gameModeForPlayer(player);
    if (!gameMode) {
        logAutoHitOnce(g_loggedAutoHitNoGameMode,
            "AutoHit has a target but no valid GameMode pointer has been observed from a real engine callback yet.");
        m_nextAutoHit = now + std::chrono::milliseconds(250);
        return;
    }

    const bool attackRouteAvailable =
        bedrocktools::core::gamehooks::attackAvailable(gameMode);
    const float hitDistance = std::sqrt(hitDistanceSq);
    const bool targetFresh =
        m_lastTargetSeen.time_since_epoch().count() != 0 &&
        now >= m_lastTargetSeen &&
        now - m_lastTargetSeen <= std::chrono::milliseconds(150);
    const bool cooldownReady =
        m_nextAutoHit.time_since_epoch().count() == 0 || now >= m_nextAutoHit;

    const bedaim::autohit::Inputs guardInput{
        m_autoHit && enabled,
        player != nullptr && player == m_lastPlayer,
        m_lastTarget != nullptr && m_targetPresent,
        validBounds(m_lastTargetBounds),
        targetFresh,
        gameMode != nullptr,
        attackRouteAvailable,
        finiteVec3(hitPoint) && std::isfinite(hitDistance),
        cooldownReady,
        hitDistance,
        autoHitRange
    };
    const auto guardDecision = bedaim::autohit::evaluate(guardInput);
    if (guardDecision != bedaim::autohit::Decision::Ready) {
        if (guardDecision == bedaim::autohit::Decision::MissingAttackRoute) {
            logAutoHitOnce(g_loggedAutoHitNoAttackRoute,
                "AutoHit skipped: no validated public GameMode::attack vtable/signature route is available.");
            m_nextAutoHit = now + std::chrono::milliseconds(250);
        }
        return;
    }

    const float delayMs = std::clamp(m_autoHitDelayMs, 80.0f, 1000.0f);
    const bool dispatched = bedrocktools::core::gamehooks::attack(
        gameMode, m_lastTarget, true, &hitPoint);
    if (!g_loggedAutoHitResult.exchange(true, std::memory_order_acq_rel)) {
        autoHitLogger().info("First AutoHit native attack attempt returned {}.", dispatched);
    }
    // Even a native rejection is rate-limited; use a short retry so a transient
    // false return doesn't permanently disable attacks for the selected target.
    const float nextDelayMs = dispatched ? delayMs : std::min(delayMs, 120.0f);
    m_nextAutoHit = now + std::chrono::milliseconds(static_cast<int>(nextDelayMs));
}

void AimAssistModule::applyAimStepLocked(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;

    if (!enabled || !player) return;
    if (!m_lastTarget || !g_applyLocalPlayerTurnDelta) return;
    m_localPlayerGliding = m_elytraMode && actorIsGliding(player);
    const auto now = std::chrono::steady_clock::now();

    auto* rotationComponent = player->rotationComponent();
    if (!rotationComponent || !validBounds(m_lastTargetBounds)) return;

    Vec2 currentRotation = field<Vec2>(rotationComponent, 0);
    if (!std::isfinite(currentRotation.x) || !std::isfinite(currentRotation.y)) return;
    currentRotation.x = std::clamp(currentRotation.x, -89.0f, 89.0f);
    currentRotation.y = wrapDegrees(currentRotation.y);

    float manualYawDelta = 0.0f;
    float manualPitchDelta = 0.0f;
    if (m_hasObservedRotation) {
        manualYawDelta = wrapDegrees(
            wrapDegrees(currentRotation.y - m_lastObservedRotation.y) - m_lastAssistYawDelta);
        manualPitchDelta = (currentRotation.x - m_lastObservedRotation.x) - m_lastAssistPitchDelta;
    }
    m_lastObservedRotation = currentRotation;
    m_hasObservedRotation = true;
    m_lastAssistYawDelta = 0.0f;
    m_lastAssistPitchDelta = 0.0f;

    const Vec3 playerPosition = player->position();
    if (!finiteVec3(playerPosition)) return;
    if (m_hasPredictionPlayerSample) {
        const float sampleDelta = std::chrono::duration<float>(
            now - m_lastPredictionPlayerSample).count();
        if (std::isfinite(sampleDelta) && sampleDelta >= 0.004f && sampleDelta <= 0.25f) {
            Vec3 velocity{
                (playerPosition.x - m_lastPredictionPlayerPosition.x) / sampleDelta,
                (playerPosition.y - m_lastPredictionPlayerPosition.y) / sampleDelta,
                (playerPosition.z - m_lastPredictionPlayerPosition.z) / sampleDelta
            };
            const float speedSquared = distanceSquared(velocity, Vec3{});
            if (std::isfinite(speedSquared) && speedSquared <= 400.0f) {
                m_playerVelocity = {
                    std::clamp(velocity.x, -20.0f, 20.0f),
                    std::clamp(velocity.y, -20.0f, 20.0f),
                    std::clamp(velocity.z, -20.0f, 20.0f)
                };
            } else {
                m_playerVelocity = {};
            }
        } else {
            m_playerVelocity = {};
        }
    } else {
        m_playerVelocity = {};
    }
    m_lastPredictionPlayerPosition = playerPosition;
    m_lastPredictionPlayerSample = now;
    m_hasPredictionPlayerSample = true;

    const AABB playerBounds = player->bounds();
    Vec3 eyePosition = playerPosition;
    if (validBounds(playerBounds)) {
        eyePosition.x = (playerBounds.min.x + playerBounds.max.x) * 0.5f;
        eyePosition.y = playerBounds.min.y + (playerBounds.max.y - playerBounds.min.y) * 0.85f;
        eyePosition.z = (playerBounds.min.z + playerBounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }

    const AABB nativeBounds = m_lastTargetBounds;
    const HeldWeapon weaponForPrediction = static_cast<HeldWeapon>(std::clamp(
        m_cachedHeldWeaponCode, 0, static_cast<int>(HeldWeapon::OtherMelee)));
    const bool rangedWeapon = weaponForPrediction == HeldWeapon::Bow ||
        weaponForPrediction == HeldWeapon::Crossbow ||
        weaponForPrediction == HeldWeapon::Trident ||
        weaponForPrediction == HeldWeapon::Spear;
    const bool usePrediction = m_predictionMode == "All" ||
        (m_predictionMode == "Ranged" && rangedWeapon);

    Vec3 predictionOffset{};
    if (usePrediction && m_hasMotionSample && m_motionTrackedTarget == m_lastTarget) {
        const float leadSeconds = std::clamp(m_predictionLeadMs, 0.0f, 300.0f) / 1000.0f;
        // Aim prediction uses target velocity relative to our own movement. A
        // moving player therefore leads their travel even when the target is still.
        predictionOffset = {
            std::clamp((m_targetVelocity.x - m_playerVelocity.x) * leadSeconds, -3.0f, 3.0f),
            std::clamp((m_targetVelocity.y - m_playerVelocity.y) * leadSeconds, -2.0f, 2.0f),
            std::clamp((m_targetVelocity.z - m_playerVelocity.z) * leadSeconds, -3.0f, 3.0f)
        };
    }

    // Prediction translates the point we aim toward; it does not modify the
    // target's stored/native bounds used by AutoHit range checks.
    AABB targetAimBounds{
        {nativeBounds.min.x + predictionOffset.x, nativeBounds.min.y + predictionOffset.y,
         nativeBounds.min.z + predictionOffset.z},
        {nativeBounds.max.x + predictionOffset.x, nativeBounds.max.y + predictionOffset.y,
         nativeBounds.max.z + predictionOffset.z}
    };
    auto* steeringExpanderBase = ModuleRegistry::get().find("bedrocktools.Hitbox Expander");
    auto* steeringExpander = (steeringExpanderBase && steeringExpanderBase->enabled)
        ? static_cast<HitboxExpanderModule*>(steeringExpanderBase) : nullptr;
    AABB bounds = nativeBounds;
    if (steeringExpander) {
        const bool targetIsPlayer = m_lastTargetIsPlayer;
        bounds = steeringExpander->expandedTargetBounds(targetIsPlayer, nativeBounds);
    }
    bounds.min.x += predictionOffset.x; bounds.max.x += predictionOffset.x;
    bounds.min.y += predictionOffset.y; bounds.max.y += predictionOffset.y;
    bounds.min.z += predictionOffset.z; bounds.max.z += predictionOffset.z;

    const Vec3 boundsCenter{
        (bounds.min.x + bounds.max.x) * 0.5f,
        (bounds.min.y + bounds.max.y) * 0.5f,
        (bounds.min.z + bounds.max.z) * 0.5f
    };
    const Vec3 centerAimPoint{
        boundsCenter.x,
        targetAimBounds.min.y + (targetAimBounds.max.y - targetAimBounds.min.y) * 0.55f,
        boundsCenter.z
    };

    const float yawRadians = currentRotation.y / kRadToDeg;
    const float pitchRadians = currentRotation.x / kRadToDeg;
    const float cosPitch = std::cos(pitchRadians);
    const Vec3 viewDirection{
        -std::sin(yawRadians) * cosPitch,
        -std::sin(pitchRadians),
        std::cos(yawRadians) * cosPitch
    };
    const float toCenterX = boundsCenter.x - eyePosition.x;
    const float toCenterY = boundsCenter.y - eyePosition.y;
    const float toCenterZ = boundsCenter.z - eyePosition.z;
    const float alongView = std::max(0.0f,
        toCenterX * viewDirection.x + toCenterY * viewDirection.y + toCenterZ * viewDirection.z);
    const Vec3 projectedPoint{
        eyePosition.x + viewDirection.x * alongView,
        eyePosition.y + viewDirection.y * alongView,
        eyePosition.z + viewDirection.z * alongView
    };

    // Distance-dependent Hitbox Area is a virtual steering volume. At close
    // range it tightens the pull; at long range the strength slider controls how
    // far the volume expands. Native Bedrock collision bounds are never changed.
    const float baseHitboxScale = std::clamp(m_hitboxScale, 1.0f, 200.0f);
    const float hitboxDistance = std::sqrt(std::max(0.0f, distanceSquared(eyePosition, boundsCenter)));
    const float hitboxScale = bedaim::aimmath::distanceHitboxScale(
        baseHitboxScale, hitboxDistance, m_range, m_hitboxDistanceGrowth,
        m_distanceDependentHitbox);
    const Vec3 halfExtents{
        (bounds.max.x - bounds.min.x) * 0.5f * hitboxScale,
        (bounds.max.y - bounds.min.y) * 0.5f * hitboxScale,
        (bounds.max.z - bounds.min.z) * 0.5f * hitboxScale
    };

    const bool stickyMode = (m_trackingMode == "Grudado");
    const bool adaptiveMode = (m_trackingMode == "Adaptativo");
    const AABB scaledAimBounds{
        {boundsCenter.x - halfExtents.x, boundsCenter.y - halfExtents.y,
         boundsCenter.z - halfExtents.z},
        {boundsCenter.x + halfExtents.x, boundsCenter.y + halfExtents.y,
         boundsCenter.z + halfExtents.z}
    };

    // In Smooth mode, do not keep steering toward the center once the crosshair
    // ray already intersects the configured hitbox area. This lets the crosshair
    // move across the hitbox naturally instead of magnetically snapping to center.
    if (!stickyMode && rayIntersectsAABB(eyePosition, viewDirection, scaledAimBounds)) {
        m_hasAimPoint = false;
        m_lastRotationUpdate = now;
        return;
    }

    const Vec3 clampedSlidePoint{
        std::clamp(projectedPoint.x, boundsCenter.x - halfExtents.x, boundsCenter.x + halfExtents.x),
        std::clamp(projectedPoint.y, boundsCenter.y - halfExtents.y, boundsCenter.y + halfExtents.y),
        std::clamp(projectedPoint.z, boundsCenter.z - halfExtents.z, boundsCenter.z + halfExtents.z)
    };
    // Never let the point fully follow the camera ray: that makes aim error collapse
    // to zero inside the hitbox and then jump when the ray exits its edge. Blending
    // from the stable center preserves a steering signal while allowing visible slide.
    constexpr float kHitboxSlideBlend = 0.42f;
    Vec3 slidePoint{
        centerAimPoint.x + (clampedSlidePoint.x - centerAimPoint.x) * kHitboxSlideBlend,
        centerAimPoint.y + (clampedSlidePoint.y - centerAimPoint.y) * kHitboxSlideBlend,
        centerAimPoint.z + (clampedSlidePoint.z - centerAimPoint.z) * kHitboxSlideBlend
    };

    const float targetDistance = std::sqrt(distanceSquared(eyePosition, centerAimPoint));
    if (m_elytraMode && m_localPlayerGliding && std::isfinite(targetDistance) &&
        targetDistance >= 7.0f && targetDistance <= 8.0f) {
        const float targetHeight = targetAimBounds.max.y - targetAimBounds.min.y;
        slidePoint.y -= std::clamp(targetHeight * 0.15f, 0.12f, 0.28f);
    }
    // The scaled AABB controls the soft-lock/sliding region, but the actual aim
    // point must remain inside Bedrock's real bounds or a hit can miss entirely.
    slidePoint.x = std::clamp(slidePoint.x, targetAimBounds.min.x, targetAimBounds.max.x);
    slidePoint.y = std::clamp(slidePoint.y, targetAimBounds.min.y, targetAimBounds.max.y);
    slidePoint.z = std::clamp(slidePoint.z, targetAimBounds.min.z, targetAimBounds.max.z);

    float deltaSeconds = 1.0f / 60.0f;
    if (m_lastRotationUpdate.time_since_epoch().count() != 0) {
        deltaSeconds = std::clamp(
            std::chrono::duration<float>(now - m_lastRotationUpdate).count(),
            0.002f, 0.05f);
    }
    m_lastRotationUpdate = now;

    if (!m_hasAimPoint) {
        m_lastAimPoint = centerAimPoint;
        m_hasAimPoint = true;
    }
    const float slideAlpha = stickyMode ? 1.0f : (1.0f - std::exp(-12.0f * deltaSeconds));
    // Preserve the established Grudado behavior. The configured virtual hitbox
    // selects the sliding point when distance-dependent hitbox is enabled; disabling
    // it restores center-lock. Smooth/Adaptativo keep their existing interpolation.
    const Vec3 effectiveAimPoint =
        (stickyMode && !m_distanceDependentHitbox) ? centerAimPoint : slidePoint;
    m_lastAimPoint.x += (effectiveAimPoint.x - m_lastAimPoint.x) * slideAlpha;
    m_lastAimPoint.y += (effectiveAimPoint.y - m_lastAimPoint.y) * slideAlpha;
    m_lastAimPoint.z += (effectiveAimPoint.z - m_lastAimPoint.z) * slideAlpha;
    m_lastAimPoint.x = std::clamp(m_lastAimPoint.x, targetAimBounds.min.x, targetAimBounds.max.x);
    m_lastAimPoint.y = std::clamp(m_lastAimPoint.y, targetAimBounds.min.y, targetAimBounds.max.y);
    m_lastAimPoint.z = std::clamp(m_lastAimPoint.z, targetAimBounds.min.z, targetAimBounds.max.z);

    const float aimDx = m_lastAimPoint.x - eyePosition.x;
    const float aimDy = m_lastAimPoint.y - eyePosition.y;
    const float aimDz = m_lastAimPoint.z - eyePosition.z;
    const float aimHorizontal = std::sqrt(aimDx * aimDx + aimDz * aimDz);
    const float desiredYaw = std::atan2(-aimDx, aimDz) * kRadToDeg;
    const float desiredPitch = -std::atan2(aimDy, std::max(aimHorizontal, 0.001f)) * kRadToDeg;
    const float yawError = wrapDegrees(desiredYaw - currentRotation.y);
    const float pitchError = desiredPitch - currentRotation.x;
    if (!std::isfinite(yawError) || !std::isfinite(pitchError)) return;

    float weaponAimMultiplier = 1.0f;
    switch (weaponForPrediction) {
        case HeldWeapon::Sword: weaponAimMultiplier = m_swordAimMultiplier; break;
        case HeldWeapon::Axe: weaponAimMultiplier = m_axeAimMultiplier; break;
        case HeldWeapon::Mace: weaponAimMultiplier = m_maceAimMultiplier; break;
        case HeldWeapon::Bow: weaponAimMultiplier = m_bowAimMultiplier; break;
        case HeldWeapon::Crossbow: weaponAimMultiplier = m_crossbowAimMultiplier; break;
        case HeldWeapon::Trident: weaponAimMultiplier = m_tridentAimMultiplier; break;
        case HeldWeapon::Spear: weaponAimMultiplier = m_spearAimMultiplier; break;
        case HeldWeapon::OtherMelee: weaponAimMultiplier = m_otherMeleeAimMultiplier; break;
        case HeldWeapon::None: break;
    }
    weaponAimMultiplier = std::clamp(weaponAimMultiplier, 0.2f, 2.0f);
    const float aimSpeed = std::clamp(m_aimSpeed * weaponAimMultiplier, 0.1f, 10.0f);
    const float rotationSpeed = std::clamp(m_rotationSpeed, 0.1f, 200.0f);
    float yawDelta = 0.0f;
    float pitchDelta = 0.0f;
    const float baseTurnRate = (70.0f + 58.0f * aimSpeed) * rotationSpeed;

    if (stickyMode) {
        // Aim profile applies in every tracking mode, including Sticky.
        float profileRate = 1.0f;
        if (m_aimMode == "Robotic") profileRate = 1.15f;
        else if (m_aimMode == "Human") profileRate = 0.78f;
        const float stickyTurnRate = baseTurnRate * profileRate;
        yawDelta = bedaim::aimmath::boundedTurnStep(yawError, stickyTurnRate, deltaSeconds);
        pitchDelta = bedaim::aimmath::boundedTurnStep(pitchError, stickyTurnRate, deltaSeconds);
    } else {
        // Smooth mode uses time-based easing with no hard-coded 30-FPS step.
        float modeResponse = 1.0f;
        float modeRate = 1.0f;
        if (m_aimMode == "Robotic") {
            modeResponse = 1.35f;
            modeRate = 1.15f;
        } else if (m_aimMode == "Human") {
            modeResponse = 0.68f;
            modeRate = 0.78f;
        }
        // Adaptive is error-aware smoothing, not target prediction: it responds
        // more decisively to large angular errors and eases down near alignment.
        const float errorMagnitude = std::sqrt(yawError * yawError + pitchError * pitchError);
        const float normalizedError = std::clamp(errorMagnitude / 12.0f, 0.0f, 1.0f);
        const float adaptiveResponse = adaptiveMode ? (0.45f + 0.55f * normalizedError) : 1.0f;
        const float adaptiveRate = adaptiveMode ? (0.65f + 0.35f * normalizedError) : 1.0f;
        const float response = (3.0f + 2.6f * aimSpeed) * modeResponse * adaptiveResponse;
        const float maxTurnRate = baseTurnRate * modeRate * adaptiveRate;
        const float alpha = 1.0f - std::exp(-response * deltaSeconds);
        yawDelta = bedaim::aimmath::boundedTurnStep(yawError * alpha, maxTurnRate, deltaSeconds);
        pitchDelta = bedaim::aimmath::boundedTurnStep(pitchError * alpha, maxTurnRate, deltaSeconds);
    }

    // Strongness is not aim speed: it controls how much the assist resists an
    // opposing manual drag. At 0, meaningful opposing input can fully suppress
    // the assist for that update; at 1, normal tracking strength is maintained.
    const float manualMagnitude = std::sqrt(
        manualYawDelta * manualYawDelta + manualPitchDelta * manualPitchDelta);
    const float errorMagnitude = std::sqrt(yawError * yawError + pitchError * pitchError);
    if (manualMagnitude > 0.12f && errorMagnitude > 0.01f &&
        std::isfinite(manualMagnitude) && std::isfinite(errorMagnitude)) {
        const float alignment = (manualYawDelta * yawError + manualPitchDelta * pitchError)
            / (manualMagnitude * errorMagnitude);
        const float opposingInput = std::clamp(-alignment, 0.0f, 1.0f);
        const float inputConfidence = std::clamp((manualMagnitude - 0.12f) / 1.0f, 0.0f, 1.0f);
        const float strongness = std::clamp(m_strength, 0.0f, 1.0f);
        const float dragScale = 1.0f - opposingInput * inputConfidence * (1.0f - strongness);
        yawDelta *= dragScale;
        pitchDelta *= dragScale;
    }

    // Naturalization normally suppresses vertical correction briefly after a jump,
    // but must not block tracking when the selected target is below the player's eyes.
    // This is common during mace Wind Burst/ground-slam arcs.
    const HeldWeapon heldWeapon = weaponForPrediction;
    const bool naturalizationForWeapon =
        m_naturalization && naturalizationAppliesToWeapon(m_naturalizationWeapon, heldWeapon);
    const bool targetBelowView = boundsCenter.y < eyePosition.y - 0.35f;
    if (naturalizationForWeapon && now < m_pitchHoldUntil && !targetBelowView)
        pitchDelta = 0.0f;

    if (!std::isfinite(yawDelta) || !std::isfinite(pitchDelta)) return;
    if (yawDelta == 0.0f && pitchDelta == 0.0f) return;

    Vec2 turnDelta{-pitchDelta, yawDelta};
    g_applyLocalPlayerTurnDelta(player, &turnDelta);
    m_lastAssistYawDelta = turnDelta.y;
    m_lastAssistPitchDelta = turnDelta.x;
}

void AimAssistModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::unique_lock<std::mutex> stateLock(m_stateMutex);
    const int previousAutoHitKeyCode = m_autoHitKeyCode.load(std::memory_order_acquire);
    const int previousAutoHitMouseButton = m_autoHitMouseButton.load(std::memory_order_acquire);
    m_targetPlayers = j.value("m_targetPlayers", m_targetPlayers);
    m_targetMobs = j.value("m_targetMobs", m_targetMobs);
    m_cantSeeThroughBlocks = j.value("m_cantSeeThroughBlocks", m_cantSeeThroughBlocks);
    m_aimMode = j.value("m_aimMode", m_aimMode);
    m_trackingMode = j.value("m_trackingMode", m_trackingMode);
    m_elytraMode = j.value("m_elytraMode", m_elytraMode);
    m_naturalization = j.value("m_naturalization", m_naturalization);
    m_naturalizationWeapon = j.value("m_naturalizationWeapon", m_naturalizationWeapon);
    m_targetSelectionMode = j.value("m_targetSelectionMode", m_targetSelectionMode);

    // LeviLauncher Radio callbacks send the selected option index ("0", "1", ...),
    // not necessarily the displayed label. Accept both the index and legacy label
    // so changes take effect immediately and older saved configs remain compatible.
    if (m_aimMode == "0") m_aimMode = "Robotic";
    else if (m_aimMode == "1") m_aimMode = "Classic";
    else if (m_aimMode == "2") m_aimMode = "Human";
    if (m_aimMode != "Robotic" && m_aimMode != "Classic" && m_aimMode != "Human") m_aimMode = "Classic";

    if (m_trackingMode == "0") m_trackingMode = "Grudado";
    else if (m_trackingMode == "1") m_trackingMode = "Smooth";
    else if (m_trackingMode == "2") m_trackingMode = "Adaptativo";
    // Some menu builds serialize the visible option label instead of its value.
    if (m_trackingMode == "Sticky" || m_trackingMode == "sticky") m_trackingMode = "Grudado";
    else if (m_trackingMode == "Adaptive" || m_trackingMode == "adaptive") m_trackingMode = "Adaptativo";
    if (m_trackingMode != "Grudado" && m_trackingMode != "Smooth" &&
        m_trackingMode != "Adaptativo") m_trackingMode = "Smooth";

    if (m_targetSelectionMode == "0") m_targetSelectionMode = "Player";
    else if (m_targetSelectionMode == "1") m_targetSelectionMode = "Crosshair";
    if (m_targetSelectionMode != "Player" && m_targetSelectionMode != "Crosshair")
        m_targetSelectionMode = "Crosshair";

    if (m_naturalizationWeapon == "0") m_naturalizationWeapon = "All";
    else if (m_naturalizationWeapon == "1") m_naturalizationWeapon = "Sword";
    else if (m_naturalizationWeapon == "2") m_naturalizationWeapon = "Axe";
    else if (m_naturalizationWeapon == "3") m_naturalizationWeapon = "Mace";
    else if (m_naturalizationWeapon == "4") m_naturalizationWeapon = "Bow";
    else if (m_naturalizationWeapon == "5") m_naturalizationWeapon = "Crossbow";
    else if (m_naturalizationWeapon == "6") m_naturalizationWeapon = "Trident";
    else if (m_naturalizationWeapon == "7") m_naturalizationWeapon = "Spear";
    else if (m_naturalizationWeapon == "8") m_naturalizationWeapon = "Other melee";
    if (m_naturalizationWeapon != "All" && m_naturalizationWeapon != "Sword" &&
        m_naturalizationWeapon != "Axe" && m_naturalizationWeapon != "Mace" &&
        m_naturalizationWeapon != "Bow" && m_naturalizationWeapon != "Crossbow" &&
        m_naturalizationWeapon != "Trident" && m_naturalizationWeapon != "Spear" &&
        m_naturalizationWeapon != "Other melee") m_naturalizationWeapon = "All";
    m_weaponOnly = j.value("m_weaponOnly", m_weaponOnly);
    m_weaponSword = j.value("m_weaponSword", m_weaponSword);
    m_weaponAxe = j.value("m_weaponAxe", m_weaponAxe);
    m_weaponMace = j.value("m_weaponMace", m_weaponMace);
    m_weaponBow = j.value("m_weaponBow", m_weaponBow);
    m_weaponCrossbow = j.value("m_weaponCrossbow", m_weaponCrossbow);
    m_weaponTrident = j.value("m_weaponTrident", m_weaponTrident);
    m_weaponSpear = j.value("m_weaponSpear", m_weaponSpear);
    m_weaponOtherMelee = j.value("m_weaponOtherMelee", m_weaponOtherMelee);
    m_range = std::clamp(j.value("m_range", m_range), 2.0f, 80.0f);
    m_fov = std::clamp(j.value("m_fov", m_fov), 1.0f, 360.0f);
    m_targetSwitchHysteresis = std::clamp(j.value("m_targetSwitchHysteresis", m_targetSwitchHysteresis), 0.0f, 12.0f);
    m_targetSwitchDistanceMargin = std::clamp(j.value("m_targetSwitchDistanceMargin", m_targetSwitchDistanceMargin), 0.0f, 5.0f);
    m_targetRetentionRadius = std::clamp(j.value("m_targetRetentionRadius", m_targetRetentionRadius), 0.5f, 5.0f);
    m_hitboxScale = std::clamp(j.value("m_hitboxScale", m_hitboxScale), 1.0f, 200.0f);
    m_distanceDependentHitbox = j.value("m_distanceDependentHitbox", m_distanceDependentHitbox);
    // A marker distinguishes old percentage-based configs from the expanded
    // scale, so values above 10 remain usable after every menu change.
    const bool usesExpandedGrowthScale = j.value("m_hitboxGrowthScaleV2", false);
    const float savedHitboxGrowth = j.value("m_hitboxDistanceGrowth", m_hitboxDistanceGrowth);
    float migratedHitboxGrowth = savedHitboxGrowth;
    if (!usesExpandedGrowthScale) {
        migratedHitboxGrowth = savedHitboxGrowth > 10.0f
            ? savedHitboxGrowth / 100.0f
            : savedHitboxGrowth;
        // Migrate the exact legacy default once. Values above 10 can now be
        // legitimate user choices in the expanded range.
        if (savedHitboxGrowth >= 99.5f && savedHitboxGrowth <= 100.5f)
            migratedHitboxGrowth = 5.0f;
    }
    m_hitboxDistanceGrowth = std::clamp(migratedHitboxGrowth, 0.1f, 200.0f);
    m_hitboxGrowthScaleV2 = true;
    m_rotationSpeed = std::clamp(j.value("m_rotationSpeed", m_rotationSpeed), 0.1f, 200.0f);
    m_predictionMode = j.value("m_predictionMode", m_predictionMode);
    if (m_predictionMode == "0") m_predictionMode = "Off";
    else if (m_predictionMode == "1") m_predictionMode = "Ranged";
    else if (m_predictionMode == "2") m_predictionMode = "All";
    if (m_predictionMode == "Ranged weapons" || m_predictionMode == "ranged weapons") m_predictionMode = "Ranged";
    else if (m_predictionMode == "All weapons" || m_predictionMode == "all weapons") m_predictionMode = "All";
    if (m_predictionMode != "Off" && m_predictionMode != "Ranged" && m_predictionMode != "All")
        m_predictionMode = "Ranged";
    m_predictionLeadMs = std::clamp(j.value("m_predictionLeadMs", m_predictionLeadMs), 0.0f, 300.0f);
    m_strength = std::clamp(j.value("m_strength", m_strength), 0.0f, 1.0f);
    m_aimSpeed = std::clamp(j.value("m_aimSpeed", m_aimSpeed), 0.1f, 10.0f);
    m_swordAimMultiplier = std::clamp(j.value("m_swordAimMultiplier", m_swordAimMultiplier), 0.2f, 2.0f);
    m_axeAimMultiplier = std::clamp(j.value("m_axeAimMultiplier", m_axeAimMultiplier), 0.2f, 2.0f);
    m_maceAimMultiplier = std::clamp(j.value("m_maceAimMultiplier", m_maceAimMultiplier), 0.2f, 2.0f);
    m_bowAimMultiplier = std::clamp(j.value("m_bowAimMultiplier", m_bowAimMultiplier), 0.2f, 2.0f);
    m_crossbowAimMultiplier = std::clamp(j.value("m_crossbowAimMultiplier", m_crossbowAimMultiplier), 0.2f, 2.0f);
    m_tridentAimMultiplier = std::clamp(j.value("m_tridentAimMultiplier", m_tridentAimMultiplier), 0.2f, 2.0f);
    m_spearAimMultiplier = std::clamp(j.value("m_spearAimMultiplier", m_spearAimMultiplier), 0.2f, 2.0f);
    m_otherMeleeAimMultiplier = std::clamp(j.value("m_otherMeleeAimMultiplier", m_otherMeleeAimMultiplier), 0.2f, 2.0f);
    m_whitelistNames = j.value("m_whitelistNames", m_whitelistNames);
    m_autoHit = j.value("m_autoHit", m_autoHit);
    m_autoHitRange = std::clamp(j.value("m_autoHitRange", m_autoHitRange), 2.0f, 6.0f);
    m_autoHitDelayMs = std::clamp(j.value("m_autoHitDelayMs", m_autoHitDelayMs), 80.0f, 1000.0f);
    int configuredKey = previousAutoHitKeyCode;
    int configuredMouseButton = previousAutoHitMouseButton;
    if (j.contains("m_autoHitKeyBinding") && j["m_autoHitKeyBinding"].is_string()) {
        m_autoHitKeyBinding = j.value("m_autoHitKeyBinding", m_autoHitKeyBinding);
        const auto parsedMouseButton = bedaim::autohitkey::parseMouseButton(m_autoHitKeyBinding);
        const auto parsedBinding = bedaim::autohitkey::parse(m_autoHitKeyBinding);
        if (parsedMouseButton.has_value()) {
            configuredMouseButton = *parsedMouseButton;
            configuredKey = 0;
        } else if (parsedBinding.has_value()) {
            configuredKey = *parsedBinding;
            configuredMouseButton = 0;
        }
        // Text edits are partial values. Unknown/incomplete aliases keep the last
        // valid binding until a complete key or mouse alias is entered.
    } else {
        configuredKey = std::clamp(j.value("m_autoHitKeyCode", previousAutoHitKeyCode), 0, 288);
        configuredMouseButton = 0;
        m_autoHitKeyBinding = bedaim::autohitkey::format(configuredKey);
    }
    m_autoHitKeyCode.store(configuredKey, std::memory_order_release);
    m_autoHitMouseButton.store(configuredMouseButton, std::memory_order_release);

    const bool keyChanged = previousAutoHitKeyCode != configuredKey;
    const bool bindingChanged = keyChanged || previousAutoHitMouseButton != configuredMouseButton;
    if (bindingChanged) {
        m_autoHitPhysicalKeyDown.store(false, std::memory_order_release);
        m_autoHitVirtualKeyDown.store(false, std::memory_order_release);
        m_autoHitMouseButtonDown.store(false, std::memory_order_release);
    }
    stateLock.unlock();
    // Mouse-button bindings keep Android keyCode at zero, so they must refresh
    // the touch Hold control when the mouse-button selection changes too.
    if (bindingChanged && m_menuRegistered.load(std::memory_order_acquire)) {
        refreshAutoHitKeyButton();
    }
}

void AimAssistModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    j["m_targetPlayers"] = m_targetPlayers;
    j["m_targetMobs"] = m_targetMobs;
    j["m_cantSeeThroughBlocks"] = m_cantSeeThroughBlocks;
    j["m_aimMode"] = m_aimMode;
    j["m_trackingMode"] = m_trackingMode;
    j["m_elytraMode"] = m_elytraMode;
    j["m_naturalization"] = m_naturalization;
    j["m_naturalizationWeapon"] = m_naturalizationWeapon;
    j["m_weaponOnly"] = m_weaponOnly;
    j["m_weaponSword"] = m_weaponSword;
    j["m_weaponAxe"] = m_weaponAxe;
    j["m_weaponMace"] = m_weaponMace;
    j["m_weaponBow"] = m_weaponBow;
    j["m_weaponCrossbow"] = m_weaponCrossbow;
    j["m_weaponTrident"] = m_weaponTrident;
    j["m_weaponSpear"] = m_weaponSpear;
    j["m_weaponOtherMelee"] = m_weaponOtherMelee;
    j["m_range"] = m_range;
    j["m_fov"] = m_fov;
    j["m_targetSwitchHysteresis"] = m_targetSwitchHysteresis;
    j["m_targetSwitchDistanceMargin"] = m_targetSwitchDistanceMargin;
    j["m_targetRetentionRadius"] = m_targetRetentionRadius;
    j["m_targetSelectionMode"] = m_targetSelectionMode;
    j["m_hitboxScale"] = m_hitboxScale;
    j["m_distanceDependentHitbox"] = m_distanceDependentHitbox;
    j["m_hitboxDistanceGrowth"] = m_hitboxDistanceGrowth;
    j["m_hitboxGrowthScaleV2"] = true;
    j["m_strength"] = m_strength;
    j["m_aimSpeed"] = m_aimSpeed;
    j["m_swordAimMultiplier"] = m_swordAimMultiplier;
    j["m_axeAimMultiplier"] = m_axeAimMultiplier;
    j["m_maceAimMultiplier"] = m_maceAimMultiplier;
    j["m_bowAimMultiplier"] = m_bowAimMultiplier;
    j["m_crossbowAimMultiplier"] = m_crossbowAimMultiplier;
    j["m_tridentAimMultiplier"] = m_tridentAimMultiplier;
    j["m_spearAimMultiplier"] = m_spearAimMultiplier;
    j["m_otherMeleeAimMultiplier"] = m_otherMeleeAimMultiplier;
    j["m_rotationSpeed"] = m_rotationSpeed;
    j["m_predictionMode"] = m_predictionMode;
    j["m_predictionLeadMs"] = m_predictionLeadMs;
    j["m_whitelistNames"] = m_whitelistNames;
    j["m_autoHit"] = m_autoHit;
    j["m_autoHitRange"] = m_autoHitRange;
    j["m_autoHitDelayMs"] = m_autoHitDelayMs;
    j["m_autoHitKeyCode"] = m_autoHitKeyCode.load(std::memory_order_acquire);
    j["m_autoHitKeyBinding"] = m_autoHitKeyBinding;
}

 
HitboxExpanderModule::HitboxExpanderModule()
    : Module("Hitbox Expander",
             "Expands client-side target selection, crosshair and AutoHit bounds; the server still validates reach and hits.") {
    keybind = 0;
}

bedrocktools::sdk::AABB HitboxExpanderModule::expandedTargetBounds(
    bool isPlayer, const bedrocktools::sdk::AABB& nativeBounds) const {
    using namespace bedrocktools::sdk;
    if (!enabled || !validBounds(nativeBounds)) return nativeBounds;

    float configuredWidth = 1.5f;
    float configuredHeight = 2.5f;
    bool selectedTypeEnabled = false;
    {
        std::lock_guard<std::mutex> lock(m_expandMutex);
        configuredWidth = std::clamp(m_hitboxWidth, 0.6f, 8.0f);
        configuredHeight = std::clamp(m_hitboxHeight, 0.6f, 8.0f);
        selectedTypeEnabled = isPlayer ? m_expandPlayers : m_expandMobs;
    }
    if (!selectedTypeEnabled) return nativeBounds;

    // Width controls X/Z and height controls Y. Preserve larger native sizes.
    const float nativeWidth = nativeBounds.max.x - nativeBounds.min.x;
    const float nativeHeight = nativeBounds.max.y - nativeBounds.min.y;
    const float nativeDepth = nativeBounds.max.z - nativeBounds.min.z;
    const float width = std::max(nativeWidth, configuredWidth);
    const float height = std::max(nativeHeight, configuredHeight);
    const float depth = std::max(nativeDepth, configuredWidth);
    const Vec3 center{
        (nativeBounds.min.x + nativeBounds.max.x) * 0.5f,
        (nativeBounds.min.y + nativeBounds.max.y) * 0.5f,
        (nativeBounds.min.z + nativeBounds.max.z) * 0.5f
    };
    const Vec3 half{width * 0.5f, height * 0.5f, depth * 0.5f};
    // Height extends upward from the target's feet; the base is not moved below
    // the floor, unlike a symmetric expansion of the vertical axis.
    return {
        {center.x - half.x, nativeBounds.min.y, center.z - half.z},
        {center.x + half.x, nativeBounds.min.y + height, center.z + half.z}
    };
}

float HitboxExpanderModule::queryExpansionMargin() const {
    std::lock_guard<std::mutex> lock(m_expandMutex);
    if (!enabled || (!m_expandPlayers && !m_expandMobs)) return 0.0f;
    return std::clamp(0.5f * std::max(
        std::clamp(m_hitboxWidth, 0.6f, 8.0f),
        std::clamp(m_hitboxHeight, 0.6f, 8.0f)), 0.0f, 4.0f);
}

bool HitboxExpanderModule::requiresLineOfSight() const {
    std::lock_guard<std::mutex> lock(m_expandMutex);
    return enabled && m_requireLineOfSight;
}

bool HitboxExpanderModule::showInLegacyMenu(std::string_view key) const {
    return key != "m_hitboxSize";
}

void HitboxExpanderModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    float legacyScale = m_hitboxSize;
    if (j.contains("m_hitboxSize") && j["m_hitboxSize"].is_number()) {
        const float value = j["m_hitboxSize"].get<float>();
        if (std::isfinite(value)) legacyScale = std::clamp(value, 1.0f, 200.0f);
    }
    legacyScale = std::clamp(legacyScale, 1.0f, 8.0f);
    float width = m_hitboxWidth;
    float height = m_hitboxHeight;
    if (j.contains("m_hitboxWidth") && j["m_hitboxWidth"].is_number()) {
        const float value = j["m_hitboxWidth"].get<float>();
        if (std::isfinite(value)) width = std::clamp(value, 0.6f, 8.0f);
    } else {
        width = std::clamp(std::max(1.5f, 0.6f * legacyScale), 0.6f, 8.0f);
    }
    if (j.contains("m_hitboxHeight") && j["m_hitboxHeight"].is_number()) {
        const float value = j["m_hitboxHeight"].get<float>();
        if (std::isfinite(value)) height = std::clamp(value, 0.6f, 8.0f);
    } else {
        height = std::clamp(std::max(2.5f, 1.8f * legacyScale), 0.6f, 8.0f);
    }
    std::lock_guard<std::mutex> lock(m_expandMutex);
    if (j.contains("m_hitboxSize") && j["m_hitboxSize"].is_number()) {
        const float value = j["m_hitboxSize"].get<float>();
        if (std::isfinite(value)) m_hitboxSize = std::clamp(value, 1.0f, 200.0f);
    }
    m_hitboxWidth = width;
    m_hitboxHeight = height;
    m_expandPlayers = j.value("m_expandPlayers", m_expandPlayers);
    m_expandMobs = j.value("m_expandMobs", m_expandMobs);
    m_requireLineOfSight = j.value("m_requireLineOfSight", m_requireLineOfSight);
}

void HitboxExpanderModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_expandMutex);
    j["m_hitboxSize"] = m_hitboxSize;
    j["m_hitboxWidth"] = m_hitboxWidth;
    j["m_hitboxHeight"] = m_hitboxHeight;
    j["m_expandPlayers"] = m_expandPlayers;
    j["m_expandMobs"] = m_expandMobs;
    j["m_requireLineOfSight"] = m_requireLineOfSight;
}
