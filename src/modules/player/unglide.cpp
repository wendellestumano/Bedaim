#include "unglide.hpp"
#include "aimassist.hpp"
#include "config/ConfigManager.hpp"
#include "MaceFallPolicy.hpp"
#include "modules/ModuleRegistry.hpp"
#include "../../core/Diagnostics.hpp"
#include "../../core/JavaVm.hpp"
#include <pl/ModMenu.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <jni.h>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
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

struct TargetGeometry {
    bool valid = false;
    float distance = std::numeric_limits<float>::infinity();
    float verticalDifference = std::numeric_limits<float>::infinity();
};

TargetGeometry targetGeometryFor(
    bedrocktools::sdk::Player* player,
    const bedrocktools::sdk::AABB& bounds) {
    using namespace bedrocktools::sdk;
    TargetGeometry result{};
    if (!player || !validBounds(bounds)) return result;

    const Vec3 eye = eyePositionFor(player);
    if (!finiteVec(eye)) return result;

    const Vec3 nearest{
        std::clamp(eye.x, bounds.min.x, bounds.max.x),
        std::clamp(eye.y, bounds.min.y, bounds.max.y),
        std::clamp(eye.z, bounds.min.z, bounds.max.z)
    };
    const float distanceSq = distanceSquared(eye, nearest);
    if (!std::isfinite(distanceSq) || distanceSq < 0.0f) return result;

    result.valid = true;
    result.distance = std::sqrt(std::max(0.0f, distanceSq));
    result.verticalDifference = std::abs(
        ((bounds.min.y + bounds.max.y) * 0.5f) - eye.y);
    return result;
}

using ActorGetStatusFlagFn = bool (*)(const void*, int);
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

// Trigger the user's dedicated Key Mapper keymap for Unglide.
// Reuse AutoHit's JNI/JavaVM and explicit-component broadcast pattern, with a
// separate UID so the Unglide action remains independent from AutoHit.
constexpr const char* kKeyMapperPackage = "io.github.sds100.keymapper";
constexpr const char* kKeyMapperReceiver =
    "io.github.sds100.keymapper.api.TriggerKeyMapsBroadcastReceiver";
constexpr const char* kKeyMapperAction =
    "io.github.sds100.keymapper.ACTION_TRIGGER_KEYMAP_BY_UID";
constexpr const char* kKeyMapperUidExtra =
    "io.github.sds100.keymapper.EXTRA_KEYMAP_UID";
constexpr const char* kUnglideKeymapUid =
    "e2225852-1df2-4232-bb26-606f11fc8745";

using JniGetCreatedJavaVMsFn = jint (*)(JavaVM**, jsize, jsize*);
enum class VmLookupStatus { Ready, SymbolUnavailable, QueryFailed, NoVmFound, AttachFailed };
JniGetCreatedJavaVMsFn resolveJniGetCreatedJavaVMs() {
    // RTLD_NOLOAD may fail under Android linker namespaces even if ART is loaded.
    static void* artHandle = dlopen("libart.so", RTLD_NOW);
    static void* helperHandle = dlopen("libnativehelper.so", RTLD_NOW);
    auto fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!fn && artHandle) fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(dlsym(artHandle, "JNI_GetCreatedJavaVMs"));
    if (!fn && helperHandle) fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(dlsym(helperHandle, "JNI_GetCreatedJavaVMs"));
    return fn;
}
class UnglideJniEnvironment {
public:
    UnglideJniEnvironment() {
        // Prefer the JavaVM provided by Preloader; dynamic lookup is a fallback
        // for runtimes that do not expose ModContext::javaVm().
        mVm = bedrocktools::core::javaVm();
        if (!mVm) {
            const auto getVms = resolveJniGetCreatedJavaVMs();
            if (!getVms) { status = VmLookupStatus::SymbolUnavailable; return; }
            JavaVM* vm = nullptr; jsize count = 0;
            if (getVms(&vm, 1, &count) != JNI_OK) { status = VmLookupStatus::QueryFailed; return; }
            if (count < 1 || !vm) { status = VmLookupStatus::NoVmFound; return; }
            mVm = vm;
        }
        const jint envStatus = mVm->GetEnv(reinterpret_cast<void**>(&mEnv), JNI_VERSION_1_6);
        if (envStatus == JNI_OK) { status = VmLookupStatus::Ready; return; }
        if (envStatus != JNI_EDETACHED) { status = VmLookupStatus::AttachFailed; return; }
        if (mVm->AttachCurrentThread(&mEnv, nullptr) == JNI_OK) { mAttached = true; status = VmLookupStatus::Ready; }
        else { mEnv = nullptr; status = VmLookupStatus::AttachFailed; }
    }
    ~UnglideJniEnvironment() { if (mAttached && mVm) mVm->DetachCurrentThread(); }
    JNIEnv* get() const noexcept { return mEnv; }
    VmLookupStatus lookupStatus() const noexcept { return status; }
private:
    JavaVM* mVm = nullptr; JNIEnv* mEnv = nullptr; bool mAttached = false;
    VmLookupStatus status = VmLookupStatus::SymbolUnavailable;
};


bool clearUnglideJniException(JNIEnv* env) {
    if (!env || !env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

enum class AutomationBroadcastResult { Sent, VmSymbolUnavailable, VmQueryFailed, VmNotFound, ThreadAttachFailed, ContextUnavailable, IntentUnavailable, BroadcastFailed };
AutomationBroadcastResult dispatchUnglideKeyMapperKeymap() {
    UnglideJniEnvironment jni;
    JNIEnv* env = jni.get();
    if (!env) {
        switch (jni.lookupStatus()) {
            case VmLookupStatus::SymbolUnavailable: return AutomationBroadcastResult::VmSymbolUnavailable;
            case VmLookupStatus::QueryFailed: return AutomationBroadcastResult::VmQueryFailed;
            case VmLookupStatus::NoVmFound: return AutomationBroadcastResult::VmNotFound;
            case VmLookupStatus::AttachFailed: return AutomationBroadcastResult::ThreadAttachFailed;
            case VmLookupStatus::Ready: break;
        }
        return AutomationBroadcastResult::VmSymbolUnavailable;
    }
    if (env->PushLocalFrame(40) != JNI_OK) { clearUnglideJniException(env); return AutomationBroadcastResult::IntentUnavailable; }
    struct LocalFrameGuard { JNIEnv* env; ~LocalFrameGuard() { env->PopLocalFrame(nullptr); } } frame{env};
    jobject app = nullptr;
    jclass threadClass = env->FindClass("android/app/ActivityThread");
    if (threadClass && !clearUnglideJniException(env)) {
        jmethodID m = env->GetStaticMethodID(threadClass, "currentApplication", "()Landroid/app/Application;");
        if (m && !clearUnglideJniException(env)) { app = env->CallStaticObjectMethod(threadClass, m); if (clearUnglideJniException(env)) app = nullptr; }
        else clearUnglideJniException(env);
    } else clearUnglideJniException(env);
    if (!app) {
        jclass globals = env->FindClass("android/app/AppGlobals");
        if (globals && !clearUnglideJniException(env)) {
            jmethodID m = env->GetStaticMethodID(globals, "getInitialApplication", "()Landroid/app/Application;");
            if (m && !clearUnglideJniException(env)) { app = env->CallStaticObjectMethod(globals, m); if (clearUnglideJniException(env)) app = nullptr; }
            else clearUnglideJniException(env);
        } else clearUnglideJniException(env);
    }
    if (!app) return AutomationBroadcastResult::ContextUnavailable;
    jclass ic = env->FindClass("android/content/Intent");
    if (!ic || clearUnglideJniException(env)) return AutomationBroadcastResult::IntentUnavailable;
    jmethodID ctor=env->GetMethodID(ic,"<init>","()V");
    jmethodID setAction=env->GetMethodID(ic,"setAction","(Ljava/lang/String;)Landroid/content/Intent;");
    jmethodID setComponent=env->GetMethodID(ic,"setComponent","(Landroid/content/ComponentName;)Landroid/content/Intent;");
    jmethodID putExtra=env->GetMethodID(ic,"putExtra","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;");
    if (clearUnglideJniException(env)||!ctor||!setAction||!setComponent||!putExtra) return AutomationBroadcastResult::IntentUnavailable;
    jclass cc=env->FindClass("android/content/ComponentName");
    if (!cc||clearUnglideJniException(env)) return AutomationBroadcastResult::IntentUnavailable;
    jmethodID cctor=env->GetMethodID(cc,"<init>","(Ljava/lang/String;Ljava/lang/String;)V");
    if (!cctor||clearUnglideJniException(env)) return AutomationBroadcastResult::IntentUnavailable;
    jobject intent=env->NewObject(ic,ctor);
    if (!intent||clearUnglideJniException(env)) return AutomationBroadcastResult::IntentUnavailable;
    jstring action=env->NewStringUTF(kKeyMapperAction), pkg=env->NewStringUTF(kKeyMapperPackage);
    jstring receiver=env->NewStringUTF(kKeyMapperReceiver), extra=env->NewStringUTF(kKeyMapperUidExtra);
    jstring uid=env->NewStringUTF(kUnglideKeymapUid);
    if (!action||!pkg||!receiver||!extra||!uid||clearUnglideJniException(env)) return AutomationBroadcastResult::IntentUnavailable;
    jobject component=env->NewObject(cc,cctor,pkg,receiver);
    if (!component||clearUnglideJniException(env)) return AutomationBroadcastResult::IntentUnavailable;
    env->CallObjectMethod(intent,setAction,action); if(clearUnglideJniException(env)) return AutomationBroadcastResult::IntentUnavailable;
    env->CallObjectMethod(intent,setComponent,component); if(clearUnglideJniException(env)) return AutomationBroadcastResult::IntentUnavailable;
    env->CallObjectMethod(intent,putExtra,extra,uid); if(clearUnglideJniException(env)) return AutomationBroadcastResult::IntentUnavailable;
    jclass context=env->FindClass("android/content/Context");
    if (!context||clearUnglideJniException(env)) return AutomationBroadcastResult::IntentUnavailable;
    jmethodID send=env->GetMethodID(context,"sendBroadcast","(Landroid/content/Intent;)V");
    if (!send||clearUnglideJniException(env)) return AutomationBroadcastResult::IntentUnavailable;
    env->CallVoidMethod(app,send,intent);
    if(clearUnglideJniException(env)) return AutomationBroadcastResult::BroadcastFailed;
    return AutomationBroadcastResult::Sent;
}
const char* automationBroadcastResultName(AutomationBroadcastResult result) {
    switch(result) {
        case AutomationBroadcastResult::Sent: return "keymapper_unglide_keymap_broadcast_sent";
        case AutomationBroadcastResult::VmSymbolUnavailable: return "android_jni_vm_symbol_unavailable";
        case AutomationBroadcastResult::VmQueryFailed: return "android_jni_vm_query_failed";
        case AutomationBroadcastResult::VmNotFound: return "android_jni_vm_not_found";
        case AutomationBroadcastResult::ThreadAttachFailed: return "android_jni_thread_attach_failed";
        case AutomationBroadcastResult::ContextUnavailable: return "android_context_unavailable";
        case AutomationBroadcastResult::IntentUnavailable: return "keymapper_unglide_intent_api_unavailable";
        case AutomationBroadcastResult::BroadcastFailed: return "keymapper_unglide_broadcast_failed";
    }
    return "keymapper_unglide_broadcast_unknown";
}

ActorGetStatusFlagFn resolveActorGetStatusFlag() {
    static ActorGetStatusFlagFn function = nullptr;
    if (!function) function = reinterpret_cast<ActorGetStatusFlagFn>(
        resolveTrustedGameSymbol("_ZNK5Actor13getStatusFlagE10ActorFlags"));
    return function;
}

bool playerIsGlidingForUnglide(void* player) {
    if (!player) return false;

    // Some Levi/Minecraft builds expose getStatusFlag but its result can be stale
    // during Elytra state transitions. Treat either independent source as evidence
    // of gliding instead of returning false from the first source unconditionally.
    const auto getStatusFlag = resolveActorGetStatusFlag();
    if (getStatusFlag) {
        try {
            if (getStatusFlag(player, 32)) return true;
        } catch (...) {
            // Try synchronized actor data below.
        }
    }
    return actorIsGliding(player);
}

}



UnglideModule::UnglideModule()
    : Module("Unglide", "Triggers one Space keymap pulse per target approach while Elytra-gliding.", "Unglide") {
    keybind = 0;
}

void UnglideModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { processPlayerTick(event.player); });
}

void UnglideModule::onEnable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_nextAttempt = {};
    m_nextDiagnostic = {};
    m_triggerArmed = true;
    m_lastTriggeredTarget = nullptr;
}

void UnglideModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_nextAttempt = {};
    m_triggerArmed = true;
    m_lastTriggeredTarget = nullptr;
}

void UnglideModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    const auto tickStarted = std::chrono::steady_clock::now();

    float targetDistance = 8.0f;
    float maximumHeightDifference = 4.0f;
    float rearmDistanceMargin = 2.0f;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!enabled || !m_unglideEnabled || !player || tickStarted < m_nextAttempt) return;
        targetDistance = m_targetDistance;
        maximumHeightDifference = m_maxVerticalDifference;
        rearmDistanceMargin = m_rearmDistanceMargin;
    }

    auto report = [&](const char* state, const std::string& detail) {
        const auto eventTime = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (eventTime < m_nextDiagnostic) return;
            m_nextDiagnostic = eventTime + std::chrono::seconds(1);
        }
        bedrocktools::diagnostics::probe(
            bedrocktools::diagnostics::Level::Debug, "Unglide", "gate", state, detail);
    };
    auto reject = [&](const char* state, const std::string& detail, int retryMs) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_nextAttempt = std::chrono::steady_clock::now() + std::chrono::milliseconds(retryMs);
        }
        report(state, detail);
    };

    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (!aimBase || !aimBase->enabled) {
        reject("aim_assist_unavailable",
               "Enable Aim Assist so Unglide can read the current player target.", 250);
        return;
    }

    auto* aim = static_cast<AimAssistModule*>(aimBase);
    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    if (!aim->currentTargetSnapshot(target, targetBounds, aimRange) || !target) {
        reject("target_missing_or_stale", "No fresh Aim Assist target; no Space keymap request sent.", 120);
        return;
    }
    if (!aim->currentTargetIsPlayerSnapshot(target)) {
        reject("target_not_player", "Unglide only triggers for a player target.", 120);
        return;
    }
    if (!validBounds(targetBounds)) {
        reject("target_bounds_invalid", "Aim Assist target bounds are invalid.", 120);
        return;
    }

    const auto geometry = targetGeometryFor(player, targetBounds);
    if (!geometry.valid || !std::isfinite(aimRange) || aimRange <= 0.0f) {
        reject("target_geometry_invalid", "Target geometry or Aim Assist range is invalid.", 120);
        return;
    }

    bool triggerArmed = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        const bool targetChanged = target != m_lastTriggeredTarget;
        if (bedaim::macefall::shouldRearmUnglide(
                m_triggerArmed, targetChanged, geometry.distance, targetDistance, rearmDistanceMargin)) {
            m_triggerArmed = true;
        }
        triggerArmed = m_triggerArmed;
        if (!triggerArmed) {
            m_nextAttempt = tickStarted + std::chrono::milliseconds(100);
        }
    }
    if (!triggerArmed) {
        report("target_approach_latched",
               "Already triggered for this approach; re-arm only after the target leaves range plus margin or target changes.");
        return;
    }

    const bool crosshairConfirmed = aim->currentTargetInCrosshair(player, target);
    if (!crosshairConfirmed) {
        reject("crosshair_not_on_target",
               "Camera ray does not intersect the selected player's hitbox; no Space keymap request sent.", 80);
        return;
    }

    const float effectiveMaximumDistance = std::min(targetDistance, aimRange);
    const bool isGliding = playerIsGlidingForUnglide(player);
    if (!bedaim::macefall::shouldPulseUnglideJump(
            true, isGliding, geometry.distance, effectiveMaximumDistance)) {
        reject(isGliding ? "target_distance_rejected" : "player_not_gliding",
               isGliding
                   ? "Target is outside the configured Unglide distance."
                   : "Gliding state was not confirmed; no Space keymap request sent.",
               180);
        return;
    }
    if (!bedaim::macefall::shouldTriggerUnglideKeymap(
            true, true, crosshairConfirmed, geometry.distance, effectiveMaximumDistance)) {
        reject("target_out_of_range",
               std::string("target_distance=") + std::to_string(geometry.distance) +
                   ", maximum=" + std::to_string(effectiveMaximumDistance), 120);
        return;
    }
    if (geometry.verticalDifference > maximumHeightDifference) {
        reject("target_height_out_of_range",
               std::string("vertical_difference=") + std::to_string(geometry.verticalDifference) +
                   ", maximum=" + std::to_string(maximumHeightDifference), 120);
        return;
    }

    // One Space pulse per approach: don't repeat every tick while the target
    // remains nearby. A new pulse is armed only when the target changes or
    // moves beyond the configured trigger distance plus the re-arm margin.
    const auto dispatchStarted = std::chrono::steady_clock::now();
    AutomationBroadcastResult result;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!enabled || !m_unglideEnabled || !m_triggerArmed ||
            dispatchStarted < m_nextAttempt) return;
        result = dispatchUnglideKeyMapperKeymap();
        if (result == AutomationBroadcastResult::Sent) {
            m_triggerArmed = false;
            m_lastTriggeredTarget = target;
            m_nextAttempt = dispatchStarted + std::chrono::milliseconds(250);
        } else {
            // Failed dispatches leave the trigger armed so a later tick can retry.
            m_nextAttempt = dispatchStarted + std::chrono::milliseconds(300);
        }
    }

    report(automationBroadcastResultName(result),
        std::string("one_shot=true; crosshair_confirmed=true; target_distance=") +
            std::to_string(geometry.distance) +
            ", vertical_difference=" + std::to_string(geometry.verticalDifference) +
            ", rearm_margin=" + std::to_string(rearmDistanceMargin) +
            ", keymap_uid=" + kUnglideKeymapUid);
}
void UnglideModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_unglideEnabled = j.value("m_unglideEnabled", m_unglideEnabled);
    m_targetDistance = std::clamp(j.value("m_unglideTargetDistance", m_targetDistance), 1.0f, 32.0f);
    m_maxVerticalDifference = std::clamp(j.value("m_unglideMaxVerticalDifference", m_maxVerticalDifference), 0.5f, 16.0f);
    m_rearmDistanceMargin = std::clamp(j.value("m_unglideRearmDistanceMargin", m_rearmDistanceMargin), 0.5f, 8.0f);
    // Legacy burst keys are intentionally ignored: Unglide is a single pulse per
    // approach now, so old saved burst counts cannot restore the repeated-trigger bug.
    m_triggerArmed = true;
    m_lastTriggeredTarget = nullptr;
}

void UnglideModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_unglideEnabled"] = m_unglideEnabled;
    j["m_unglideTargetDistance"] = m_targetDistance;
    j["m_unglideMaxVerticalDifference"] = m_maxVerticalDifference;
    j["m_unglideRearmDistanceMargin"] = m_rearmDistanceMargin;
}

void UnglideModule::onMenuRegistered() {
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

    schema.defaultCategory("special").category(
        "special", "Special", "Elytra Macing — one Unglide pulse per target approach");

    ConfigNodeV2 enabledNode;
    enabledNode.id = "m_unglideEnabled";
    enabledNode.key = enabledNode.id;
    enabledNode.title = "Enable Elytra Macing";
    enabledNode.description = "Send one Space keymap pulse when gliding near the selected player.";
    enabledNode.category = "special";
    enabledNode.type = ConfigControlTypeV2::Toggle;
    enabledNode.defaultValue = "false";
    enabledNode.currentValue = valueString(enabledNode.key.c_str(), "false");
    enabledNode.hasCurrentValue = true;
    schema.node(std::move(enabledNode));

    auto addSlider = [&](const char* key, const char* title, const char* minimum,
                         const char* maximum, const char* step, const char* unit,
                         const char* fallback, const char* description) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.title = title;
        node.description = description;
        node.category = "special";
        node.type = ConfigControlTypeV2::SliderFloat;
        node.defaultValue = fallback;
        node.currentValue = valueString(key, fallback);
        node.hasCurrentValue = true;
        node.minValue = minimum;
        node.maxValue = maximum;
        node.step = step;
        node.unit = unit;
        schema.node(std::move(node));
    };

    addSlider("m_unglideTargetDistance", "Trigger distance", "1", "32", "0.5",
              " blocks", "8.0", "Distance to the selected player that triggers the single pulse.");
    addSlider("m_unglideMaxVerticalDifference", "Maximum height difference", "0.5", "16", "0.5",
              " blocks", "4.0", "Reject targets that are too far above or below the player.");
    addSlider("m_unglideRearmDistanceMargin", "Re-arm distance margin", "0.5", "8", "0.5",
              " blocks", "2.0", "The target must move this much beyond trigger distance before another pulse is allowed.");

    ConfigNodeV2 keybindNode;
    keybindNode.id = "keybind";
    keybindNode.key = "keybind";
    keybindNode.title = "Keybind";
    keybindNode.category = "special";
    keybindNode.type = ConfigControlTypeV2::Keybind;
    schema.node(std::move(keybindNode));
    setConfigSchemaJson(moduleId, schema.toJson());
}

bool UnglideModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    bool recognized = true;
    if (key == "m_unglideEnabled") {
        const bool parsed = value == "true" || value == "1" || value == "on";
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_unglideEnabled = parsed;
        if (!parsed) {
            m_triggerArmed = true;
            m_lastTriggeredTarget = nullptr;
        }
    } else {
        float number = 0.0f;
        try {
            std::size_t parsedLength = 0;
            const std::string text(value);
            number = std::stof(text, &parsedLength);
            while (parsedLength < text.size() &&
                   std::isspace(static_cast<unsigned char>(text[parsedLength]))) ++parsedLength;
            if (parsedLength != text.size() || !std::isfinite(number)) return false;
        } catch (...) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (key == "m_unglideTargetDistance") {
            m_targetDistance = std::clamp(number, 1.0f, 32.0f);
            m_triggerArmed = true;
            m_lastTriggeredTarget = nullptr;
        } else if (key == "m_unglideMaxVerticalDifference") {
            m_maxVerticalDifference = std::clamp(number, 0.5f, 16.0f);
        } else if (key == "m_unglideRearmDistanceMargin") {
            m_rearmDistanceMargin = std::clamp(number, 0.5f, 8.0f);
        } else {
            recognized = false;
        }
    }
    if (!recognized) return false;

    // Persist immediately; merely updating runtime fields lets the old schema
    // reappear with stale values when the LeviLauncher menu closes/reopens.
    bedrocktools::config::ConfigManager::get().save();
    onMenuRegistered(); // Rebuild currentValue from the just-updated module state.
    return true;
}
