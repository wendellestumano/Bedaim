#include "unglide.hpp"
#include "aimassist.hpp"
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



UnglideModule::UnglideModule():Module("Unglide","Triggers the dedicated Key Mapper Space keymap near a target.","Unglide"){keybind=0;}
void UnglideModule::onInit(){bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([this](auto& e){processPlayerTick(e.player);});}
void UnglideModule::onEnable(){std::lock_guard<std::mutex> l(m_stateMutex);m_nextAttempt={};m_nextDiagnostic={};m_burstRemaining=0;m_triggerState=TriggerState::Idle;m_triggeredTarget=nullptr;}
void UnglideModule::onDisable(){std::lock_guard<std::mutex> l(m_stateMutex);m_nextAttempt={};m_burstRemaining=0;m_triggerState=TriggerState::Idle;m_triggeredTarget=nullptr;}
void UnglideModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    const auto tickStarted = std::chrono::steady_clock::now();

    float targetDistance = 8.0f;
    float maximumHeightDifference = 4.0f;
    int burstCount = 3;
    int pressIntervalMs = 120;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!enabled || !m_unglideEnabled || !player || tickStarted < m_nextAttempt) return;
        targetDistance = m_targetDistance;
        maximumHeightDifference = m_maxVerticalDifference;
        burstCount = m_burstCount;
        pressIntervalMs = m_pressIntervalMs;
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
            m_burstRemaining = 0;
            m_triggerState = TriggerState::Idle;
            m_triggeredTarget = nullptr;
            m_nextAttempt = std::chrono::steady_clock::now() + std::chrono::milliseconds(retryMs);
        }
        report(state, detail);
    };

    // Match AutoHit's targeting contract: use the fresh Aim Assist target,
    // require that it is a player, and verify the camera ray intersects that
    // same target. A nearby player elsewhere on screen is not sufficient.
    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (!aimBase || !aimBase->enabled) {
        reject("aim_assist_unavailable",
               "Enable Aim Assist so Unglide can verify the currently targeted player.", 250);
        return;
    }

    auto* aim = static_cast<AimAssistModule*>(aimBase);
    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    if (!aim->currentTargetSnapshot(target, targetBounds, aimRange) || !target) {
        reject("target_missing_or_stale",
               "No fresh Aim Assist target is available; no Space keymap request sent.", 120);
        return;
    }
    if (!aim->currentTargetIsPlayerSnapshot(target)) {
        reject("target_not_player",
               "Unglide only triggers for a player target, not a mob target.", 120);
        return;
    }
    if (!validBounds(targetBounds)) {
        reject("target_bounds_invalid",
               "Aim Assist target bounds are invalid; no Space keymap request sent.", 120);
        return;
    }
    const bool crosshairConfirmed = aim->currentTargetInCrosshair(player, target);
    const auto geometry = targetGeometryFor(player, targetBounds);
    if (!geometry.valid || !std::isfinite(aimRange) || aimRange <= 0.0f) {
        reject("target_geometry_invalid",
               "Target geometry or Aim Assist range is invalid; no Space keymap request sent.", 120);
        return;
    }
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
                   ? "Target distance rejected by Unglide pulse policy."
                   : "Local player is not gliding; Space keymap request suppressed.",
               250);
        return;
    }
    if (!bedaim::macefall::shouldTriggerUnglideKeymap(
            true, true, crosshairConfirmed, geometry.distance, effectiveMaximumDistance)) {
        if (!crosshairConfirmed) {
            reject("crosshair_not_on_target",
                   "Camera ray does not intersect the selected player's hitbox; no Space keymap request sent.", 80);
        } else {
            reject("target_out_of_range",
                   std::string("target_distance=") + std::to_string(geometry.distance) +
                       ", maximum=" + std::to_string(effectiveMaximumDistance), 120);
        }
        return;
    }
    if (geometry.verticalDifference > maximumHeightDifference) {
        reject("target_height_out_of_range",
               std::string("vertical_difference=") + std::to_string(geometry.verticalDifference) +
                   ", maximum=" + std::to_string(maximumHeightDifference), 120);
        return;
    }

    const auto dispatchStarted = std::chrono::steady_clock::now();
    AutomationBroadcastResult result = AutomationBroadcastResult::BroadcastFailed;
    int remaining = 0;
    bool alreadyTriggered = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!enabled || !m_unglideEnabled || dispatchStarted < m_nextAttempt) return;

        // A target change starts a new approach. The same target can trigger
        // only one bounded burst until a gate rejects it or the target is lost.
        if (m_triggeredTarget != target) {
            m_triggeredTarget = target;
            m_triggerState = TriggerState::Tracking;
            m_burstRemaining = 0;
        }
        alreadyTriggered = m_triggerState == TriggerState::Triggered;
        if (!bedaim::macefall::shouldDispatchUnglideBurst(alreadyTriggered, m_burstRemaining)) {
            m_nextAttempt = dispatchStarted + std::chrono::milliseconds(80);
            return;
        }
        if (m_triggerState != TriggerState::Triggered) {
            m_triggerState = TriggerState::Triggered;
            m_burstRemaining = burstCount;
        }

        // Keep the native state lock across dispatch, matching the previous
        // lifecycle guarantee that a disable cannot race into a new request.
        result = dispatchUnglideKeyMapperKeymap();
        const bool sent = result == AutomationBroadcastResult::Sent;
        if (sent) --m_burstRemaining;
        else m_burstRemaining = 0;
        remaining = m_burstRemaining;
        m_nextAttempt = dispatchStarted + std::chrono::milliseconds(
            sent ? (remaining > 0 ? pressIntervalMs : 160) : 300);
    }

    report(automationBroadcastResultName(result),
        std::string("state=triggered; crosshair_confirmed=true; target_distance=") + std::to_string(geometry.distance) +
            ", vertical_difference=" + std::to_string(geometry.verticalDifference) +
            ", burst_remaining=" + std::to_string(remaining) +
            ", keymap_uid=" + kUnglideKeymapUid);
}
void UnglideModule::loadConfig(const nlohmann::json& j){Module::loadConfig(j);std::lock_guard<std::mutex> l(m_stateMutex);m_unglideEnabled=j.value("m_unglideEnabled",m_unglideEnabled);m_targetDistance=std::clamp(j.value("m_unglideTargetDistance",m_targetDistance),1.0f,32.0f);m_maxVerticalDifference=std::clamp(j.value("m_unglideMaxVerticalDifference",m_maxVerticalDifference),0.5f,16.0f);m_burstCount=std::clamp(j.value("m_unglideBurstCount",m_burstCount),1,10);m_pressIntervalMs=std::clamp(j.value("m_unglidePressIntervalMs",m_pressIntervalMs),50,1000);}
void UnglideModule::saveConfig(nlohmann::json& j){Module::saveConfig(j);std::lock_guard<std::mutex> l(m_stateMutex);j["m_unglideEnabled"]=m_unglideEnabled;j["m_unglideTargetDistance"]=m_targetDistance;j["m_unglideMaxVerticalDifference"]=m_maxVerticalDifference;j["m_unglideBurstCount"]=m_burstCount;j["m_unglidePressIntervalMs"]=m_pressIntervalMs;}
void UnglideModule::onMenuRegistered(){
 using namespace pl::modmenu;ConfigSchemaBuilder s;nlohmann::json c;saveConfig(c);auto vs=[&](const char*k,const char*d)->std::string{auto i=c.find(k);if(i==c.end()||i->is_null())return d;if(i->is_string())return i->get<std::string>();if(i->is_boolean())return i->get<bool>()?"true":"false";return i->dump();};
 s.defaultCategory("special").category("special","Special","Independent Unglide trigger");
 ConfigNodeV2 t;t.id="m_unglideEnabled";t.key=t.id;t.title="Enable Unglide";t.category="special";t.type=ConfigControlTypeV2::Toggle;t.defaultValue="false";t.currentValue=vs(t.key.c_str(),"false");t.hasCurrentValue=true;s.node(std::move(t));
 auto sl=[&](const char*k,const char*title,const char*mn,const char*mx,const char*step,const char*unit,const char*def){ConfigNodeV2 n;n.id=k;n.key=k;n.title=title;n.category="special";n.type=ConfigControlTypeV2::SliderFloat;n.defaultValue=def;n.currentValue=vs(k,def);n.hasCurrentValue=true;n.minValue=mn;n.maxValue=mx;n.step=step;n.unit=unit;s.node(std::move(n));};
 sl("m_unglideTargetDistance","Target distance","1","32","0.5"," blocks","8.0");sl("m_unglideMaxVerticalDifference","Maximum height difference","0.5","16","0.5"," blocks","4.0");sl("m_unglideBurstCount","Burst presses","1","10","1","","3");sl("m_unglidePressIntervalMs","Time between presses","50","1000","10"," ms","120");
 ConfigNodeV2 k;k.id="keybind";k.key="keybind";k.title="Keybind";k.category="special";k.type=ConfigControlTypeV2::Keybind;s.node(std::move(k));setConfigSchemaJson(moduleId,s.toJson());
}
bool UnglideModule::onMenuConfigChanged(std::string_view k,std::string_view v){
 if(k=="m_unglideEnabled"){bool b=v=="true"||v=="1"||v=="on";std::lock_guard<std::mutex> l(m_stateMutex);m_unglideEnabled=b;return true;}
 try{std::size_t p=0;std::string x(v);float n=std::stof(x,&p);while(p<x.size()&&std::isspace(static_cast<unsigned char>(x[p])))++p;if(p!=x.size()||!std::isfinite(n))return false;std::lock_guard<std::mutex> l(m_stateMutex);if(k=="m_unglideTargetDistance")m_targetDistance=std::clamp(n,1.0f,32.0f);else if(k=="m_unglideMaxVerticalDifference")m_maxVerticalDifference=std::clamp(n,0.5f,16.0f);else if(k=="m_unglideBurstCount")m_burstCount=std::clamp(static_cast<int>(std::lround(n)),1,10);else if(k=="m_unglidePressIntervalMs")m_pressIntervalMs=std::clamp(static_cast<int>(std::lround(n)),50,1000);else return false;return true;}catch(...){return false;}
}
