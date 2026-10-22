#include "maceswapper.hpp"
#include "MaceFallPolicy.hpp"
#include "../../core/Diagnostics.hpp"
#include "swapperstate.hpp"

#include <pl/ModMenu.hpp>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include "aimassist.hpp"
#include "core/InventoryAccess.hpp"
#include "core/GameHooks.hpp"
#include "../ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/memory/Signatures.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <jni.h>
#include <limits>
#include <vector>

namespace {
bool finiteVec(const bedrocktools::sdk::Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool validBounds(const bedrocktools::sdk::AABB& bounds) {
    if (!finiteVec(bounds.min) || !finiteVec(bounds.max)) return false;
    const float height = bounds.max.y - bounds.min.y;
    const float width = bounds.max.x - bounds.min.x;
    const float depth = bounds.max.z - bounds.min.z;
    return height >= 0.25f && height <= 8.0f && width > 0.0f && width <= 5.0f
        && depth > 0.0f && depth <= 5.0f;
}

float distanceSquared(const bedrocktools::sdk::Vec3& a, const bedrocktools::sdk::Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bedrocktools::sdk::Vec3 eyePositionFor(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    const Vec3 position = player->position();
    const AABB bounds = player->bounds();
    Vec3 eyePosition = position;
    if (validBounds(bounds)) {
        eyePosition.x = (bounds.min.x + bounds.max.x) * 0.5f;
        eyePosition.y = bounds.min.y + (bounds.max.y - bounds.min.y) * 0.85f;
        eyePosition.z = (bounds.min.z + bounds.max.z) * 0.5f;
    } else {
        eyePosition.y += 1.5f;
    }
    return eyePosition;
}

bool targetWithinRange(bedrocktools::sdk::Player* player,
                       const bedrocktools::sdk::AABB& targetBounds,
                       float range) {
    if (!player || !validBounds(targetBounds)) return false;
    const auto eye = eyePositionFor(player);
    if (!finiteVec(eye)) return false;
    const bedrocktools::sdk::Vec3 hitPoint{
        std::clamp(eye.x, targetBounds.min.x, targetBounds.max.x),
        std::clamp(eye.y, targetBounds.min.y, targetBounds.max.y),
        std::clamp(eye.z, targetBounds.min.z, targetBounds.max.z)
    };
    const float distanceSq = distanceSquared(eye, hitPoint);
    const float safeRange = std::clamp(range, 2.0f, 6.0f);
    // Overlap with the player's eye still counts as in range.
    return std::isfinite(distanceSq) && distanceSq <= safeRange * safeRange;
}

float distanceToTargetBounds(bedrocktools::sdk::Player* player,
                              const bedrocktools::sdk::AABB& targetBounds) {
    if (!player || !validBounds(targetBounds))
        return std::numeric_limits<float>::infinity();
    const auto eye = eyePositionFor(player);
    if (!finiteVec(eye)) return std::numeric_limits<float>::infinity();
    const bedrocktools::sdk::Vec3 nearest{
        std::clamp(eye.x, targetBounds.min.x, targetBounds.max.x),
        std::clamp(eye.y, targetBounds.min.y, targetBounds.max.y),
        std::clamp(eye.z, targetBounds.min.z, targetBounds.max.z)
    };
    const float distanceSq = distanceSquared(eye, nearest);
    return std::isfinite(distanceSq)
        ? std::sqrt(std::max(0.0f, distanceSq))
        : std::numeric_limits<float>::infinity();
}

using GetRuntimeActorListFn = std::vector<void*> (*)(void*);
using ActorIsPlayerFn = bool (*)(void*);
using ActorGetStatusFlagFn = bool (*)(const void*, int);

// Unglide resolves its own nearby-player snapshot; it does not depend on the
// Aim Assist module toggle or the camera-tracking module's cached target.
struct NearbyPlayerSearch {
    bool queryReady = false;
    bool found = false;
    float distance = std::numeric_limits<float>::infinity();
};

NearbyPlayerSearch nearestNearbyPlayerDistance(
    bedrocktools::sdk::Player* player, float maximumDistance) {
    using namespace bedrocktools::sdk;
    NearbyPlayerSearch result{};
    if (!player) return result;

    static GetRuntimeActorListFn actorList = nullptr;
    static ActorIsPlayerFn actorIsPlayer = nullptr;
    if (!actorList) actorList = reinterpret_cast<GetRuntimeActorListFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList));
    if (!actorIsPlayer) actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    if (!actorList || !actorIsPlayer) return result;

    auto* localActor = reinterpret_cast<Actor*>(player);
    auto* level = localActor->level();
    void* manager = level ? level->actorManager() : nullptr;
    if (!manager) return result;

    std::vector<void*> actors;
    try {
        actors = actorList(manager);
    } catch (...) {
        return result;
    }
    if (actors.size() > 4096) return result;
    result.queryReady = true;

    const Vec3 eye = eyePositionFor(player);
    if (!finiteVec(eye)) {
        result.queryReady = false;
        return result;
    }
    const float range = std::clamp(
        std::isfinite(maximumDistance) ? maximumDistance : 8.0f, 1.0f, 32.0f);

    for (void* candidate : actors) {
        if (!candidate || candidate == player ||
            reinterpret_cast<std::uintptr_t>(candidate) <= 0x1000) continue;

        bool isPlayer = false;
        try {
            isPlayer = actorIsPlayer(candidate);
        } catch (...) {
            continue;
        }
        if (!isPlayer) continue;

        auto* target = reinterpret_cast<Actor*>(candidate);
        if (target->level() != level) continue;
        const AABB bounds = target->bounds();
        if (!validBounds(bounds)) continue;

        const Vec3 nearest{
            std::clamp(eye.x, bounds.min.x, bounds.max.x),
            std::clamp(eye.y, bounds.min.y, bounds.max.y),
            std::clamp(eye.z, bounds.min.z, bounds.max.z)
        };
        const float distanceSq = distanceSquared(eye, nearest);
        if (!std::isfinite(distanceSq) || distanceSq > range * range) continue;
        const float distance = std::sqrt(std::max(0.0f, distanceSq));
        if (!result.found || distance < result.distance) {
            result.found = true;
            result.distance = distance;
        }
    }
    return result;
}

bool actorIsGliding(void* actor) {
    using namespace bedrocktools::sdk;
    if (!actor) return false;

    const auto actorAddress = reinterpret_cast<std::uintptr_t>(actor);
    if (actorAddress <= 0x1000 ||
        actorAddress > std::numeric_limits<std::uintptr_t>::max() -
            offsets::Actor::mEntityData - sizeof(void*)) return false;

    const auto wrapperAddress = actorAddress + offsets::Actor::mEntityData;
    void* component = nullptr;
    std::memcpy(&component, reinterpret_cast<const void*>(wrapperAddress), sizeof(component));
    if (reinterpret_cast<std::uintptr_t>(component) <= 0x1000) return false;

    const auto componentAddress = reinterpret_cast<std::uintptr_t>(component);
    auto** begin = *reinterpret_cast<void***>(componentAddress);
    auto** end = *reinterpret_cast<void***>(componentAddress + sizeof(void*));
    if (!begin || !end) return false;

    const auto beginAddress = reinterpret_cast<std::uintptr_t>(begin);
    const auto endAddress = reinterpret_cast<std::uintptr_t>(end);
    if (endAddress < beginAddress ||
        (endAddress - beginAddress) % sizeof(void*) != 0 ||
        (endAddress - beginAddress) / sizeof(void*) > 192) return false;

    const auto count = (endAddress - beginAddress) / sizeof(void*);
    for (std::size_t index = 0; index < count; ++index) {
        void* item = begin[index];
        if (reinterpret_cast<std::uintptr_t>(item) <= 0x1000) continue;
        const auto itemAddress = reinterpret_cast<std::uintptr_t>(item);
        std::uint8_t type = 0;
        std::uint16_t id = 0;
        std::memcpy(&type, reinterpret_cast<const void*>(itemAddress + offsets::DataItem::mType), sizeof(type));
        std::memcpy(&id, reinterpret_cast<const void*>(itemAddress + offsets::DataItem::mId), sizeof(id));
        if (type != 7 || id != 0) continue;

        std::uint64_t flags = 0;
        std::memcpy(&flags, reinterpret_cast<const void*>(itemAddress + offsets::DataItem::mValue), sizeof(flags));
        constexpr std::uint64_t kGlidingFlag = std::uint64_t{1} << 32;
        return (flags & kGlidingFlag) != 0;
    }
    return false;
}

bool isTrustedGameProvider(void* address) {
    if (!address) return false;
    Dl_info info{};
    if (dladdr(address, &info) == 0 || !info.dli_fname) return false;
    const std::string_view path(info.dli_fname);
    return path.find("libminecraftpe.so") != std::string_view::npos ||
        path.find("liblevilamina.so") != std::string_view::npos ||
        path.find("libLeviLamina.so") != std::string_view::npos;
}

void* resolveTrustedGameSymbol(const char* symbol) {
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
    if (isTrustedGameProvider(address)) return address;
    if (minecraftHandle) {
        address = dlsym(minecraftHandle, symbol);
        if (isTrustedGameProvider(address)) return address;
    }
    if (leviLaminaHandle) {
        address = dlsym(leviLaminaHandle, symbol);
        if (isTrustedGameProvider(address)) return address;
    }
    if (leviLaminaAltHandle) {
        address = dlsym(leviLaminaAltHandle, symbol);
        if (isTrustedGameProvider(address)) return address;
    }
    return nullptr;
}

// MacroDroid exposes an "Intent Received" trigger for custom broadcasts.
// Configure it with this action and a single "Input Key Event: Space" action.
// MacroDroid then uses its supported input/Accessibility/Shizuku path; Bedaim
// only emits the trigger intent and does not inject input or install additional hooks.
constexpr const char* kMacroDroidPackage = "com.arlosoft.macrodroid";
constexpr const char* kBedaimSpacebarIntentAction =
    "com.wendellestumano.bedaim.UNGLIDE_SPACEBAR";

using JniGetCreatedJavaVMsFn = jint (*)(JavaVM**, jsize, jsize*);

JniGetCreatedJavaVMsFn resolveJniGetCreatedJavaVMs() {
    static void* artHandle = dlopen("libart.so", RTLD_NOW | RTLD_NOLOAD);
    static auto getVms = reinterpret_cast<JniGetCreatedJavaVMsFn>(
        dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!getVms && artHandle) {
        getVms = reinterpret_cast<JniGetCreatedJavaVMsFn>(
            dlsym(artHandle, "JNI_GetCreatedJavaVMs"));
    }
    return getVms;
}

class ScopedJniEnvironment {
public:
    ScopedJniEnvironment() {
        const auto getVms = resolveJniGetCreatedJavaVMs();
        if (!getVms) return;

        JavaVM* vm = nullptr;
        jsize vmCount = 0;
        if (getVms(&vm, 1, &vmCount) != JNI_OK || vmCount < 1 || !vm) return;
        mVm = vm;

        const jint status = mVm->GetEnv(reinterpret_cast<void**>(&mEnv), JNI_VERSION_1_6);
        if (status == JNI_OK) return;
        if (status != JNI_EDETACHED) {
            mEnv = nullptr;
            return;
        }

        if (mVm->AttachCurrentThread(&mEnv, nullptr) == JNI_OK) {
            mAttached = true;
        } else {
            mEnv = nullptr;
        }
    }

    ~ScopedJniEnvironment() {
        if (mAttached && mVm) mVm->DetachCurrentThread();
    }

    JNIEnv* get() const noexcept { return mEnv; }

private:
    JavaVM* mVm = nullptr;
    JNIEnv* mEnv = nullptr;
    bool mAttached = false;
};

class ScopedJniLocalFrame {
public:
    explicit ScopedJniLocalFrame(JNIEnv* env) : mEnv(env) {
        mReady = mEnv && mEnv->PushLocalFrame(32) == JNI_OK;
        if (!mReady && mEnv && mEnv->ExceptionCheck()) mEnv->ExceptionClear();
    }
    ~ScopedJniLocalFrame() {
        if (mReady) mEnv->PopLocalFrame(nullptr);
    }
    bool ready() const noexcept { return mReady; }
private:
    JNIEnv* mEnv = nullptr;
    bool mReady = false;
};

enum class AutomationBroadcastResult {
    Sent,
    JavaVmUnavailable,
    ApplicationContextUnavailable,
    IntentApiUnavailable,
    BroadcastFailed,
};

bool clearJniException(JNIEnv* env) {
    if (!env || !env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

AutomationBroadcastResult dispatchMacroDroidSpacebarMacro() {
    ScopedJniEnvironment jni;
    JNIEnv* env = jni.get();
    if (!env) return AutomationBroadcastResult::JavaVmUnavailable;
    ScopedJniLocalFrame localFrame(env);
    if (!localFrame.ready()) return AutomationBroadcastResult::IntentApiUnavailable;

    jobject application = nullptr;
    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if (activityThread && !clearJniException(env)) {
        jmethodID currentApplication = env->GetStaticMethodID(
            activityThread, "currentApplication", "()Landroid/app/Application;");
        if (currentApplication && !clearJniException(env)) {
            application = env->CallStaticObjectMethod(activityThread, currentApplication);
            if (clearJniException(env)) application = nullptr;
        } else {
            clearJniException(env);
        }
    } else {
        clearJniException(env);
    }

    // Some Android builds expose this equivalent accessor more reliably.
    if (!application) {
        jclass appGlobals = env->FindClass("android/app/AppGlobals");
        if (appGlobals && !clearJniException(env)) {
            jmethodID initialApplication = env->GetStaticMethodID(
                appGlobals, "getInitialApplication", "()Landroid/app/Application;");
            if (initialApplication && !clearJniException(env)) {
                application = env->CallStaticObjectMethod(appGlobals, initialApplication);
                if (clearJniException(env)) application = nullptr;
            } else {
                clearJniException(env);
            }
        } else {
            clearJniException(env);
        }
    }
    if (!application) return AutomationBroadcastResult::ApplicationContextUnavailable;

    jclass intentClass = env->FindClass("android/content/Intent");
    if (!intentClass || clearJniException(env))
        return AutomationBroadcastResult::IntentApiUnavailable;

    jmethodID intentConstructor = env->GetMethodID(intentClass, "<init>", "()V");
    jmethodID setAction = env->GetMethodID(
        intentClass, "setAction", "(Ljava/lang/String;)Landroid/content/Intent;");
    jmethodID setPackage = env->GetMethodID(
        intentClass, "setPackage", "(Ljava/lang/String;)Landroid/content/Intent;");
    if (clearJniException(env) || !intentConstructor || !setAction || !setPackage)
        return AutomationBroadcastResult::IntentApiUnavailable;

    jobject intent = env->NewObject(intentClass, intentConstructor);
    if (!intent || clearJniException(env))
        return AutomationBroadcastResult::IntentApiUnavailable;

    jstring action = env->NewStringUTF(kBedaimSpacebarIntentAction);
    jstring packageName = env->NewStringUTF(kMacroDroidPackage);
    if (!action || !packageName || clearJniException(env))
        return AutomationBroadcastResult::IntentApiUnavailable;

    env->CallObjectMethod(intent, setAction, action);
    if (clearJniException(env)) return AutomationBroadcastResult::IntentApiUnavailable;
    env->CallObjectMethod(intent, setPackage, packageName);
    if (clearJniException(env)) return AutomationBroadcastResult::IntentApiUnavailable;

    jclass contextClass = env->FindClass("android/content/Context");
    if (!contextClass || clearJniException(env))
        return AutomationBroadcastResult::IntentApiUnavailable;
    jmethodID sendBroadcast = env->GetMethodID(
        contextClass, "sendBroadcast", "(Landroid/content/Intent;)V");
    if (!sendBroadcast || clearJniException(env))
        return AutomationBroadcastResult::IntentApiUnavailable;

    env->CallVoidMethod(application, sendBroadcast, intent);
    if (clearJniException(env)) return AutomationBroadcastResult::BroadcastFailed;
    return MoonBroadcastResult::Sent;
}

const char* moonBroadcastResultName(MoonBroadcastResult result) {
    switch (result) {
        case MoonBroadcastResult::Sent: return "keymapper_broadcast_sent";
        case AutomationBroadcastResult::JavaVmUnavailable: return "android_java_vm_unavailable";
        case AutomationBroadcastResult::ApplicationContextUnavailable: return "android_context_unavailable";
        case AutomationBroadcastResult::IntentApiUnavailable: return "android_intent_api_unavailable";
        case AutomationBroadcastResult::BroadcastFailed: return "keymapper_broadcast_failed";
    }
    return "keymapper_broadcast_unknown";
}

ActorGetStatusFlagFn resolveActorGetStatusFlag() {
    static ActorGetStatusFlagFn function = nullptr;
    if (!function) function = reinterpret_cast<ActorGetStatusFlagFn>(
        resolveTrustedGameSymbol("_ZNK5Actor13getStatusFlagE10ActorFlags"));
    return function;
}

bool playerIsGlidingForUnglide(void* player) {
    if (!player) return false;
    const auto getStatusFlag = resolveActorGetStatusFlag();
    if (getStatusFlag) {
        try {
            // ActorFlags::Gliding is 32 in the public Bedrock/LeviLamina enum.
            return getStatusFlag(player, 32);
        } catch (...) {
            // Fall through to the existing synchronized-data reader.
        }
    }
    return actorIsGliding(player);
}

}

MaceSwapperModule::MaceSwapperModule()
    : Module("MaceFall", "Selects the mace for a falling hit when the BedrockAim target is in the crosshair.",
             "Mace Swapper") {
    keybind = 0;
}

void MaceSwapperModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) {
            processPlayerTick(event.player);
        });
}

void MaceSwapperModule::onEnable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    m_peakY = 0.0f;
    m_hasPeakY = false;
    m_nextAttack = {};
    m_nextUnglideAttempt = {};
    m_nextUnglideDiagnostic = {};

    // A rapid disable/re-enable can happen before the next player tick. Preserve
    // pending slot ownership so that tick can restore the old slot; never drop
    // the restoration state in a UI callback.
    if (m_pendingAttack) {
        m_cancelPendingAttack = true;
    } else {
        swapperstate::maceFallPriority.store(false, std::memory_order_release);
        m_pendingMaceSlot = -1;
        m_pendingPreviousSlot = -1;
        m_pendingTarget = nullptr;
        m_pendingPlayer = nullptr;
        m_cancelPendingAttack = false;
    }
}

void MaceSwapperModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    // If we were between the hotbar change and the hit, cancel the hit but leave
    // restoration pending for the next player tick.
    if (m_pendingAttack) m_cancelPendingAttack = true;
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    m_peakY = 0.0f;
    m_hasPeakY = false;
    m_nextAttack = {};
    m_nextUnglideAttempt = {};
    m_nextUnglideDiagnostic = {};
}

void MaceSwapperModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> lock(m_stateMutex);

    if (!player) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "MaceSwapper", "player", "missing");
        m_hasLastPlayerY = false;
        m_hasPeakY = false;
        // A missing-player tick is not proof that the pending swap can be
        // discarded: the inventory may still be on the mace. Mark the pending
        // attack for cancellation but retain its ownership/slot snapshot until
        // a valid player tick can safely restore it.
        if (m_pendingAttack) {
            m_cancelPendingAttack = true;
        } else {
            m_cancelPendingAttack = false;
            m_pendingMaceSlot = -1;
            m_pendingPreviousSlot = -1;
            m_pendingTarget = nullptr;
            m_pendingPlayer = nullptr;
            swapperstate::maceFallPriority.store(false, std::memory_order_release);
        }
        return;
    }

    const Vec3 playerPosition = player->position();
    if (!finiteVec(playerPosition)) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "MaceSwapper", "player_position", "invalid");
        m_hasLastPlayerY = false;
        return;
    }

    const float verticalDelta = m_hasLastPlayerY ? playerPosition.y - m_lastPlayerY : 0.0f;
    m_lastPlayerY = playerPosition.y;
    m_hasLastPlayerY = true;

    // Track the peak of the current airborne segment. Rebase on level/rising
    // motion so an earlier jump cannot arm a later fall.
    if (!m_hasPeakY || verticalDelta >= -0.025f) {
        m_peakY = playerPosition.y;
        m_hasPeakY = true;
    }
    if (playerPosition.y > m_peakY) m_peakY = playerPosition.y;
    const float fallDistance = std::max(0.0f, m_peakY - playerPosition.y);

    auto& inventory = bedrocktools::core::InventoryAccess::get();

    // Finish a swap on the following tick. This gives vanilla a tick to observe
    // the selected mace before attack and slot restoration, rather than changing
    // the selected slot twice in the same callback where no equip update can occur.
    if (m_pendingAttack) {
        const int selectedSlot = inventory.selectedHotbarSlot(player);
        const bool stillHoldingMace = selectedSlot == m_pendingMaceSlot;
        auto restoreIfStillSwapped = [&]() {
            if (m_restoreSlot && stillHoldingMace && m_pendingPreviousSlot >= 0) {
                inventory.selectHotbarSlot(player, m_pendingPreviousSlot);
            }
            m_pendingAttack = false;
            m_cancelPendingAttack = false;
            swapperstate::maceFallPriority.store(false, std::memory_order_release);
            m_pendingMaceSlot = -1;
            m_pendingPreviousSlot = -1;
            m_pendingTarget = nullptr;
            m_pendingPlayer = nullptr;
        };

        if (m_cancelPendingAttack || !enabled || !m_enableMaceSwapper) {
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
                "MaceSwapper", "pending_attack", "cancelled_or_disabled");
            restoreIfStillSwapped();
            return;
        }
        if (player != m_pendingPlayer) {
            // Never restore the previous player's hotbar index into a different
            // player's inventory after a respawn/world transition.
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
                "MaceSwapper", "pending_attack", "player_changed_abandon_old_inventory");
            m_pendingAttack = false;
            m_cancelPendingAttack = false;
            swapperstate::maceFallPriority.store(false, std::memory_order_release);
            m_pendingMaceSlot = -1;
            m_pendingPreviousSlot = -1;
            m_pendingTarget = nullptr;
            m_pendingPlayer = nullptr;
            return;
        }
        if (!stillHoldingMace) {
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
                "MaceSwapper", "pending_attack", "selected_slot_changed");
            restoreIfStillSwapped();
            return;
        }

        auto* aimModuleBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
        if (!aimModuleBase || !aimModuleBase->enabled) {
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
                "MaceSwapper", "aim_dependency", "disabled_during_pending_attack");
            restoreIfStillSwapped();
            return;
        }
        auto* aim = static_cast<AimAssistModule*>(aimModuleBase);
        void* currentTarget = nullptr;
        AABB targetBounds{};
        float aimRange = 0.0f;
        if (!aim->currentTargetSnapshot(currentTarget, targetBounds, aimRange) ||
            currentTarget != m_pendingTarget ||
            !targetWithinRange(player, targetBounds, std::min(m_attackRange, aimRange))) {
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
                "MaceSwapper", "pending_target", "missing_stale_or_out_of_range");
            restoreIfStillSwapped();
            return;
        }

        void* gameMode = bedrocktools::core::gamehooks::gameMode();
        if (!gameMode) {
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
                "MaceSwapper", "gamemode", "unavailable");
            restoreIfStillSwapped();
            return;
        }

        const Vec3 eye = eyePositionFor(player);
        const Vec3 hitPoint{
            std::clamp(eye.x, targetBounds.min.x, targetBounds.max.x),
            std::clamp(eye.y, targetBounds.min.y, targetBounds.max.y),
            std::clamp(eye.z, targetBounds.min.z, targetBounds.max.z)
        };
        const float intervalMs = std::clamp(m_attackIntervalMs, 80.0f, 1000.0f);
        const bool attacked = bedrocktools::core::gamehooks::attack(gameMode, currentTarget, true, &hitPoint);
        bedrocktools::diagnostics::probe(attacked ? bedrocktools::diagnostics::Level::Info :
                bedrocktools::diagnostics::Level::Warning,
            "MaceSwapper", "attack_result", attacked ? "accepted" : "rejected",
            "native attack wrapper result after deferred mace selection");
        restoreIfStillSwapped();
        if (attacked) {
            m_nextAttack = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(static_cast<int>(intervalMs));
        }
        return;
    }

    if (!enabled) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "MaceSwapper", "action_gate", "module_disabled");
        return;
    }

    // Unglide asks MacroDroid to perform the Android Space key event through its
    // supported Input Key Event action. MacroDroid must have the Intent Received
    // trigger and privileged input route configured by the user.
    if (m_unglideEnabled) {
        const auto now = std::chrono::steady_clock::now();
        auto reportUnglideGate = [&](const char* state, const std::string& details) {
            if (now < m_nextUnglideDiagnostic) return;
            m_nextUnglideDiagnostic = now + std::chrono::seconds(1);
            bedrocktools::diagnostics::probe(
                bedrocktools::diagnostics::Level::Debug, "MaceSwapper", "unglide_gate", state, details);
        };

        if (now >= m_nextUnglideAttempt) {
            if (!playerIsGlidingForUnglide(player)) {
                m_nextUnglideAttempt = now + std::chrono::milliseconds(250);
                reportUnglideGate("not_gliding", "The native gliding flag is not set.");
            } else {
                const auto target = nearestNearbyPlayerDistance(player, m_unglideTargetDistance);
                if (!target.queryReady) {
                    m_nextUnglideAttempt = now + std::chrono::milliseconds(1000);
                    reportUnglideGate("target_lookup_unavailable",
                        "ActorManagerList or ActorIsPlayer signature, level, or actor snapshot unavailable.");
                } else if (!target.found) {
                    m_nextUnglideAttempt = now + std::chrono::milliseconds(250);
                    reportUnglideGate("no_nearby_player",
                        std::string("No other player inside ") +
                            std::to_string(m_unglideTargetDistance) + " blocks.");
                } else if (!bedaim::macefall::shouldPulseUnglideJump(
                               m_unglideEnabled, true, target.distance, m_unglideTargetDistance)) {
                    m_nextUnglideAttempt = now + std::chrono::milliseconds(250);
                    reportUnglideGate("target_out_of_range", "Closest player exceeds the configured distance.");
                } else {
                    const auto result = dispatchMacroDroidSpacebarMacro();
                    // A successful sendBroadcast confirms Android accepted the
                    // request, not that a MacroDroid macro/input route was active.
                    reportUnglideGate(
                        automationBroadcastResultName(result),
                        std::string("target_distance=") + std::to_string(target.distance) +
                            ", action=" + kBedaimSpacebarIntentAction +
                            ", configure MacroDroid Intent Received + Input Key Event Space, and enable its supported Shizuku/root input route.");
                    // Keep repeats at >=1s because Android may throttle background broadcasts.
                    m_nextUnglideAttempt = now + std::chrono::milliseconds(
                        result == AutomationBroadcastResult::Sent ? 1000 : 1500);
                    return;
                }
            }
        }
    }

    if (!m_enableMaceSwapper) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "MaceSwapper", "attack_gate", "mace_swapper_disabled");
        return;
    }

    if (!bedaim::macefall::shouldBeginMaceAction(verticalDelta, fallDistance, m_minimumFallDistance)) {
        const char* fallState = verticalDelta >= -0.025f ? "not_descending" : "fall_below_minimum";
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "MaceSwapper", "fall_gate", fallState,
            std::string("fall_distance=") + std::to_string(fallDistance));
        return;
    }
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
        "MaceSwapper", "fall_gate", "armed");

    const auto now = std::chrono::steady_clock::now();
    if (m_nextAttack.time_since_epoch().count() != 0 && now < m_nextAttack) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "MaceSwapper", "cooldown", "waiting");
        return;
    }

    auto* aimModuleBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (!aimModuleBase || !aimModuleBase->enabled) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "MaceSwapper", "aim_dependency", "disabled");
        return;
    }
    auto* aim = static_cast<AimAssistModule*>(aimModuleBase);

    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    if (!aim->currentTargetSnapshot(target, targetBounds, aimRange)) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "MaceSwapper", "target", "missing_or_stale");
        return;
    }
    if (!aim->currentTargetInCrosshair(player, target)) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "MaceSwapper", "crosshair", "target_not_centered");
        return;
    }
    if (!targetWithinRange(player, targetBounds, std::min(m_attackRange, aimRange))) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "MaceSwapper", "range", "out_of_range");
        return;
    }

    void* gameMode = bedrocktools::core::gamehooks::gameMode();
    if (!gameMode) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "MaceSwapper", "gamemode", "unavailable");
        return;
    }

    const int maceSlot = inventory.findHotbarSlot(player, "mace");
    const int previousSlot = inventory.selectedHotbarSlot(player);
    if (maceSlot < 0 || previousSlot < 0) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "MaceSwapper", "inventory", "mace_or_selected_slot_unavailable");
        return;
    }

    if (previousSlot != maceSlot && !inventory.selectHotbarSlot(player, maceSlot)) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "MaceSwapper", "inventory", "mace_selection_failed");
        return;
    }
    m_pendingMaceSlot = maceSlot;
    m_pendingPreviousSlot = previousSlot;
    m_cancelPendingAttack = false;
    m_pendingTarget = target;
    m_pendingPlayer = player;
    m_pendingAttack = true;
    swapperstate::maceFallPriority.store(true, std::memory_order_release);
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "MaceSwapper", "pending_attack", "armed",
        std::string("mace_slot=") + std::to_string(maceSlot) +
        ", previous_slot=" + std::to_string(previousSlot));
}

void MaceSwapperModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_enableMaceSwapper = j.value("m_enableMaceSwapper", m_enableMaceSwapper);
    m_attackRange = std::clamp(j.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
    m_minimumFallDistance = bedaim::macefall::normalizeMinimumFallDistance(j.value("m_minimumFallDistance", m_minimumFallDistance));
    m_attackIntervalMs = std::clamp(j.value("m_attackIntervalMs", m_attackIntervalMs), 80.0f, 1000.0f);
    m_restoreSlot = j.value("m_restoreSlot", m_restoreSlot);
    m_unglideEnabled = j.value("m_unglideEnabled", m_unglideEnabled);
    m_unglideTargetDistance = std::clamp(
        j.value("m_unglideTargetDistance", m_unglideTargetDistance), 1.0f, 32.0f);
}

void MaceSwapperModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_enableMaceSwapper"] = m_enableMaceSwapper;
    j["m_attackRange"] = m_attackRange;
    j["m_minimumFallDistance"] = bedaim::macefall::normalizeMinimumFallDistance(m_minimumFallDistance);
    j["m_attackIntervalMs"] = m_attackIntervalMs;
    j["m_restoreSlot"] = m_restoreSlot;
    j["m_unglideEnabled"] = m_unglideEnabled;
    j["m_unglideTargetDistance"] = m_unglideTargetDistance;
}

void MaceSwapperModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    nlohmann::json config;
    saveConfig(config);
    auto valueString = [&](const char* key, const char* fallback) -> std::string {
        const auto it = config.find(key);
        if (it == config.end() || it->is_null()) return fallback;
        if (it->is_string()) return it->get<std::string>();
        if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
        return it->dump();
    };
    schema.defaultCategory("combat")
        .category("combat", "Combat", "Mace swap and fall requirements")
        .category("special", "Special", "Elytra Unglide conditions");

    auto slider = [&](const char* key, const char* title,
                      const char* minimum, const char* maximum,
                      const char* step, const char* unit, const char* category,
                      const char* defaultValue) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.title = title;
        node.category = category;
        node.type = ConfigControlTypeV2::SliderFloat;
        node.defaultValue = defaultValue;
        node.currentValue = valueString(key, defaultValue);
        node.hasCurrentValue = true;
        node.minValue = minimum;
        node.maxValue = maximum;
        node.step = step;
        node.unit = unit;
        schema.node(std::move(node));
    };

    ConfigNodeV2 enabledNode;
    enabledNode.id = "m_enableMaceSwapper";
    enabledNode.key = "m_enableMaceSwapper";
    enabledNode.title = "Enable Mace Swapper";
    enabledNode.category = "combat";
    enabledNode.type = ConfigControlTypeV2::Toggle;
    enabledNode.defaultValue = "true";
    enabledNode.currentValue = valueString("m_enableMaceSwapper", "true");
    enabledNode.hasCurrentValue = true;
    schema.node(std::move(enabledNode));

    slider("m_attackRange", "Target range", "2", "6", "0.1", " blocks", "combat", "3.0");
    slider("m_minimumFallDistance", "Minimum fall distance", "0.30", "10.0", "0.10", " blocks", "combat", "1.55");
    slider("m_attackIntervalMs", "Attack interval", "80", "1000", "10", " ms", "combat", "180.0");

    ConfigNodeV2 unglideEnabledNode;
    unglideEnabledNode.id = "m_unglideEnabled";
    unglideEnabledNode.key = "m_unglideEnabled";
    unglideEnabledNode.title = "Enable Unglide";
    unglideEnabledNode.description =
        "Stop Elytra gliding only when a target is nearby and a solid floor is within the configured clearance.";
    unglideEnabledNode.category = "special";
    unglideEnabledNode.type = ConfigControlTypeV2::Toggle;
    unglideEnabledNode.defaultValue = "false";
    unglideEnabledNode.currentValue = valueString("m_unglideEnabled", "false");
    unglideEnabledNode.hasCurrentValue = true;
    schema.node(std::move(unglideEnabledNode));

    slider("m_unglideTargetDistance", "Unglide target distance", "1", "32", "0.5", " blocks", "special", "8.0");

    ConfigNodeV2 restoreNode;
    restoreNode.id = "m_restoreSlot";
    restoreNode.key = "m_restoreSlot";
    restoreNode.title = "Restore previous hotbar slot";
    restoreNode.category = "combat";
    restoreNode.type = ConfigControlTypeV2::Toggle;
    restoreNode.defaultValue = "true";
    restoreNode.currentValue = valueString("m_restoreSlot", "true");
    restoreNode.hasCurrentValue = true;
    schema.node(std::move(restoreNode));

    ConfigNodeV2 keybindNode;
    keybindNode.id = "keybind";
    keybindNode.key = "keybind";
    keybindNode.title = "Keybind";
    keybindNode.category = "combat";
    keybindNode.type = ConfigControlTypeV2::Keybind;
    schema.node(std::move(keybindNode));
    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

bool MaceSwapperModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "m_enableMaceSwapper" || key == "m_restoreSlot" ||
        key == "m_unglideEnabled") {
        const bool flag = value == "true" || value == "1" || value == "on";
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (key == "m_enableMaceSwapper") m_enableMaceSwapper = flag;
        else if (key == "m_restoreSlot") m_restoreSlot = flag;
        else m_unglideEnabled = flag;
        return true;
    }
    try {
        std::size_t parsedLength = 0;
        const std::string text(value);
        const float number = std::stof(text, &parsedLength);
        while (parsedLength < text.size() &&
               std::isspace(static_cast<unsigned char>(text[parsedLength]))) ++parsedLength;
        if (parsedLength != text.size() || !std::isfinite(number)) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (key == "m_attackRange") m_attackRange = std::clamp(number, 2.0f, 6.0f);
        else if (key == "m_minimumFallDistance")
            m_minimumFallDistance = bedaim::macefall::normalizeMinimumFallDistance(number);
        else if (key == "m_attackIntervalMs") m_attackIntervalMs = std::clamp(number, 80.0f, 1000.0f);
        else if (key == "m_unglideTargetDistance")
            m_unglideTargetDistance = std::clamp(number, 1.0f, 32.0f);
        else return false;
        return true;
    } catch (...) {
        return false;
    }
}
