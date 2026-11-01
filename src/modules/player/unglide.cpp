#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "core/memory/MinecraftBuildIdentity.hpp"
#include "unglide.hpp"
#include "aimassist.hpp"
#include "MaceFallPolicy.hpp"
#include "UnglideActorFlagReader.hpp"
#include "modules/ModuleRegistry.hpp"
#include "../../core/Diagnostics.hpp"
#include "../../core/JavaVm.hpp"
#include "../../config/ConfigManager.hpp"
#include <pl/ModMenu.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/input/MoveInput.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <jni.h>
#include <link.h>
#include <limits>
#include <optional>
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

// Exact Minecraft 26.60 EnTT pool identifier recovered from its registered
// component descriptor. Query the registry by this ID rather than by a C++ type
// name: the descriptor ID and EnTT's compiler-derived type hash are different.
constexpr entt::id_type kGlidingTravelFlagComponentId = 0x27a011eeu;

std::optional<bool> playerHasGlidingTravelTag(void* player) {
    using namespace bedrocktools::sdk;
    if (!player || !bedrocktools::core::memory::isExactMinecraft260Build())
        return std::nullopt;

    const auto playerAddress = reinterpret_cast<std::uintptr_t>(player);
    if (offsets::Actor::mEntityContext >
        std::numeric_limits<std::uintptr_t>::max() - playerAddress) return std::nullopt;
    auto* context = reinterpret_cast<::EntityContext*>(
        playerAddress + offsets::Actor::mEntityContext);
    if (!context) return std::nullopt;

    // Separate movement/travel-state telemetry; not the source of truth for
    // Bedrock's Molang query.is_gliding predicate.
    auto* componentPool = context->getRegistry().storage(kGlidingTravelFlagComponentId);
    if (!componentPool) return std::nullopt;
    return componentPool->contains(context->mEntity);
}

using ActorDataFlagTestFn = bool (*)(void*, int);

struct MinecraftImageBaseQuery {
    std::uintptr_t base{};
};

int findExactMinecraftImageBase(struct dl_phdr_info* info, std::size_t, void* opaque) {
    if (!info || !info->dlpi_name) return 0;
    const std::string_view imageName(info->dlpi_name);
    const auto slash = imageName.find_last_of('/');
    const auto basename = slash == std::string_view::npos
        ? imageName : imageName.substr(slash + 1);
    if (basename != "libminecraftpe.so") return 0;

    bedrocktools::core::memory::detail::MinecraftBuildSearch build{};
    if (!bedrocktools::core::memory::detail::inspectMinecraftImage(info, 0, &build) ||
        !build.matched) return 0;

    auto& query = *static_cast<MinecraftImageBaseQuery*>(opaque);
    query.base = static_cast<std::uintptr_t>(info->dlpi_addr);
    return query.base ? 1 : 0;
}

struct MinecraftExecutableRangeQuery {
    std::uintptr_t base{};
    std::uintptr_t address{};
    std::size_t length{};
    bool found{};
};

int inspectExactMinecraftExecutableRange(struct dl_phdr_info* info, std::size_t, void* opaque) {
    if (!info || !info->dlpi_name) return 0;
    const std::string_view imageName(info->dlpi_name);
    const auto slash = imageName.find_last_of('/');
    const auto basename = slash == std::string_view::npos
        ? imageName : imageName.substr(slash + 1);
    auto& query = *static_cast<MinecraftExecutableRangeQuery*>(opaque);
    if (basename != "libminecraftpe.so" ||
        static_cast<std::uintptr_t>(info->dlpi_addr) != query.base) return 0;

    const auto maximum = std::numeric_limits<std::uintptr_t>::max();
    if (query.length == 0 || query.length > maximum - query.address) return 0;
    for (ElfW(Half) index = 0; index < info->dlpi_phnum; ++index) {
        const auto& segment = info->dlpi_phdr[index];
        if (segment.p_type != PT_LOAD ||
            (segment.p_flags & (PF_R | PF_X)) != (PF_R | PF_X)) continue;
        const auto offset = static_cast<std::uintptr_t>(segment.p_vaddr);
        const auto size = static_cast<std::uintptr_t>(segment.p_memsz);
        if (offset > maximum - query.base) continue;
        const auto begin = query.base + offset;
        if (size > maximum - begin) continue;
        const auto end = begin + size;
        if (query.address >= begin && query.address < end &&
            query.length <= end - query.address) {
            query.found = true;
            return 1;
        }
    }
    return 0;
}

bool isExactMinecraftExecutableRange(std::uintptr_t base, std::uintptr_t address,
                                     std::size_t length) {
    if (!base || !address || length == 0 ||
        length > std::numeric_limits<std::uintptr_t>::max() - address) return false;
    MinecraftExecutableRangeQuery query{base, address, length, false};
    dl_iterate_phdr(&inspectExactMinecraftExecutableRange, &query);
    return query.found;
}

ActorDataFlagTestFn resolveActorDataFlagTest() {
    static std::atomic<ActorDataFlagTestFn> cached{nullptr};
    if (const auto existing = cached.load(std::memory_order_acquire)) return existing;
    if (!bedrocktools::core::memory::isExactMinecraft260Build()) return nullptr;

    MinecraftImageBaseQuery image{};
    dl_iterate_phdr(&findExactMinecraftImageBase, &image);
    if (!image.base) return nullptr;

    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    if (bedaim::macefall::actor_flag::kReaderRva > maximum - image.base ||
        bedaim::macefall::actor_flag::kHelperRva > maximum - image.base) return nullptr;
    const auto readerAddress = image.base + bedaim::macefall::actor_flag::kReaderRva;
    const auto helperAddress = image.base + bedaim::macefall::actor_flag::kHelperRva;

    if (!isExactMinecraftExecutableRange(image.base, readerAddress,
                                          sizeof(bedaim::macefall::actor_flag::kReaderInstructions)) ||
        !isExactMinecraftExecutableRange(image.base, helperAddress,
                                          sizeof(bedaim::macefall::actor_flag::kHelperInstructions)))
        return nullptr;

    std::array<std::uint32_t, 2> readerInstructions{};
    std::array<std::uint32_t, 4> helperInstructions{};
    std::memcpy(readerInstructions.data(), reinterpret_cast<const void*>(readerAddress),
                sizeof(readerInstructions));
    std::memcpy(helperInstructions.data(), reinterpret_cast<const void*>(helperAddress),
                sizeof(helperInstructions));

    if (!bedaim::macefall::actor_flag::matchesReaderStub(readerInstructions) ||
        !bedaim::macefall::actor_flag::readerBranchesToHelper(readerInstructions) ||
        !bedaim::macefall::actor_flag::matchesHelperPrefix(helperInstructions)) {
        bedrocktools::diagnostics::probe(
            bedrocktools::diagnostics::Level::Warning, "Unglide", "actor_flag_reader",
            "26.60_signature_mismatch",
            "query.is_gliding helper prologue or ActorDataFlagComponent hash did not match the verified 26.60 instructions");
        return nullptr;
    }

    const auto function = reinterpret_cast<ActorDataFlagTestFn>(readerAddress);
    cached.store(function, std::memory_order_release);
    bedrocktools::diagnostics::probe(
        bedrocktools::diagnostics::Level::Info, "Unglide", "actor_flag_reader",
        "resolved",
        "26.60 query.is_gliding helper verified: ActorDataFlagComponent hash 0xC67426F3, flag bit 32");
    return function;
}

std::optional<bool> playerIsGlidingFromActorDataFlags(void* player) {
    if (!player || !bedrocktools::core::memory::isExactMinecraft260Build())
        return std::nullopt;
    const auto reader = resolveActorDataFlagTest();
    if (!reader) return std::nullopt;

    // This is the exact lower-level predicate called by the 26.60 Molang
    // query.is_gliding lambda: Actor* + bit index 32. Do not call the Molang
    // lambda itself; it requires a live RenderParams/vector context.
    return reader(player, bedaim::macefall::actor_flag::kGlidingFlagBit);
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
        // Match AutoHit's known-working Key Mapper route: ask ART for the
        // process JavaVM first, then fall back to the Preloader-cached VM.
        VmLookupStatus lookupFailure = VmLookupStatus::SymbolUnavailable;
        const auto getVms = resolveJniGetCreatedJavaVMs();
        if (getVms) {
            JavaVM* vm = nullptr;
            jsize count = 0;
            const jint query = getVms(&vm, 1, &count);
            if (query == JNI_OK && count > 0 && vm) {
                mVm = vm;
            } else if (query != JNI_OK) {
                lookupFailure = VmLookupStatus::QueryFailed;
            } else {
                lookupFailure = VmLookupStatus::NoVmFound;
            }
        }
        if (!mVm) mVm = bedrocktools::core::javaVm();
        if (!mVm) {
            status = lookupFailure;
            return;
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


}



UnglideModule::UnglideModule():Module("Unglide","Triggers the dedicated Key Mapper Space keymap near a target.","Unglide"){keybind=0;}
void UnglideModule::onInit() {
    auto& events = bedrocktools::events::bus();
    events.subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { cacheLocalPlayer(event.player); });
    // ClientInstanceUpdateEvent is published after the native ClientInstance
    // update returns, so ECS travel-state membership is sampled after the
    // engine's travel systems have had an opportunity to update the registry.
    events.subscribe<bedrocktools::events::ClientInstanceUpdateEvent>(
        [this](auto&) { processClientUpdate(); });
}
void UnglideModule::onEnable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_nextAttempt = {};
    m_nextDiagnostic = {};
    m_burstRemaining = 0;
    m_latchedTarget = nullptr;
    m_targetLatched = false;
    m_triggerState = TriggerState::Idle;
    m_lastPlayer = nullptr;
}
void UnglideModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_nextAttempt = {};
    m_burstRemaining = 0;
    m_latchedTarget = nullptr;
    m_targetLatched = false;
    m_triggerState = TriggerState::Idle;
    m_lastPlayer = nullptr;
}
void UnglideModule::cacheLocalPlayer(bedrocktools::sdk::Player* player) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastPlayer = player;
}

void UnglideModule::processClientUpdate() {
    bedrocktools::sdk::Player* player = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        player = m_lastPlayer;
    }
    processPlayerUpdate(player);
}

void UnglideModule::processPlayerUpdate(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    const auto tickStarted = std::chrono::steady_clock::now();

    // Sample the post-update callback before any feature gate returns. This
    // distinguishes a missing event subscription from a legitimate gliding gate.
    static std::atomic<std::uint64_t> heartbeatTicks{0};
    static std::atomic<std::int64_t> nextHeartbeatMs{0};
    const auto tickCount = heartbeatTicks.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        tickStarted.time_since_epoch()).count();

    float targetDistance = 8.0f;
    float maximumHeightDifference = 4.0f;
    bool moduleEnabled = false;
    bool featureEnabled = false;
    bool dispatchInProgress = false;
    bool retryPending = false;
    std::int64_t retryRemainingMs = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        moduleEnabled = enabled;
        featureEnabled = m_unglideEnabled;
        dispatchInProgress = m_dispatchInProgress;
        retryPending = m_nextAttempt.time_since_epoch().count() != 0 &&
            tickStarted < m_nextAttempt;
        if (retryPending) {
            retryRemainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                m_nextAttempt - tickStarted).count();
        }
        targetDistance = m_targetDistance;
        maximumHeightDifference = m_maxVerticalDifference;
    }

    auto expectedHeartbeat = nextHeartbeatMs.load(std::memory_order_relaxed);
    if (nowMs >= expectedHeartbeat && nextHeartbeatMs.compare_exchange_strong(
            expectedHeartbeat, nowMs + 1000, std::memory_order_relaxed)) {
        bedrocktools::diagnostics::log(
            bedrocktools::diagnostics::Level::Info, "Unglide",
            std::string("client_update_heartbeat=callback_received: update_count=") +
                std::to_string(tickCount) +
                ", module_enabled=" + (moduleEnabled ? "true" : "false") +
                ", feature_enabled=" + (featureEnabled ? "true" : "false") +
                ", player_non_null=" + (player ? "true" : "false") +
                ", dispatch_in_progress=" + (dispatchInProgress ? "true" : "false") +
                ", retry_pending=" + (retryPending ? "true" : "false") +
                ", retry_remaining_ms=" + std::to_string(retryRemainingMs) +
                ", player_address=" +
                std::to_string(reinterpret_cast<std::uintptr_t>(player)));
    }

    if (!moduleEnabled || !featureEnabled || !player || dispatchInProgress || retryPending)
        return;

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
            const bool preserveTriggeredLatch = bedaim::macefall::shouldKeepUnglideTriggerLatch(
                m_triggerState == TriggerState::Triggered && m_targetLatched,
                false, false, false);
            // A transient gate failure cancels the remaining burst but must not
            // re-arm the same approach. Reset target state before reject() when
            // target identity/data is genuinely lost or invalid.
            m_burstRemaining = 0;
            if (!preserveTriggeredLatch) {
                m_targetLatched = false;
                m_triggerState = m_latchedTarget ? TriggerState::Tracking : TriggerState::Idle;
            }
            m_nextAttempt = std::chrono::steady_clock::now() + std::chrono::milliseconds(retryMs);
        }
        report(state, detail);
    };
    auto resetTargetState = [&]() {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_burstRemaining = 0;
        m_latchedTarget = nullptr;
        m_targetLatched = false;
        m_triggerState = TriggerState::Idle;
    };

    // Gate every native target/crosshair query, not only the later ECS read.
    // This prevents BedrockTools signatures or offsets from being used when
    // the loaded libminecraftpe.so is not the exact 26.60 baseline.
    if (!bedrocktools::core::memory::isExactMinecraft260Build()) {
        resetTargetState();
        reject("unsupported_native_baseline",
               "Loaded libminecraftpe.so does not match the required Minecraft 26.60 Build ID; no native target query or Key Mapper request sent.",
               1000);
        return;
    }

    // Unglide performs its own fresh player/crosshair query. Aim Assist may be
    // disabled or keybind-blocked; its native actor-query signatures are initialized
    // independently and this read-only helper does not steer the camera.
    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (!aimBase) {
        resetTargetState();
        reject("targeting_service_unavailable",
               "The shared player-query service is not registered; no Space keymap request sent.", 250);
        return;
    }
    auto* aim = static_cast<AimAssistModule*>(aimBase);
    void* target = nullptr;
    AABB targetBounds{};
    if (!aim->findCrosshairPlayerSnapshot(player, targetDistance, target, targetBounds) || !target) {
        resetTargetState();
        reject("crosshair_player_not_found",
               "No valid player hitbox intersects the camera ray inside Unglide's configured distance.", 120);
        return;
    }
    const auto geometry = targetGeometryFor(player, targetBounds);
    if (!geometry.valid) {
        resetTargetState();
        reject("target_geometry_invalid",
               "The fresh crosshair player's geometry is invalid; no Space keymap request sent.", 120);
        return;
    }

    const float effectiveMaximumDistance = targetDistance;
    // Match Bedrock's own Molang query.is_gliding implementation. On the exact
    // 26.60 binary, its lambda passes Actor* and bit 32 to the helper that looks
    // up ActorDataFlagComponent (hash 0xC67426F3). The separate
    // GlidingTravelFlagComponent is retained only as timing/consistency telemetry.
    const auto actorDataGliding = playerIsGlidingFromActorDataFlags(player);
    const auto glidingTravelTag = playerHasGlidingTravelTag(player);
    if (!bedaim::macefall::shouldDispatchUnglideWhenGliding(
            actorDataGliding.has_value(), actorDataGliding.value_or(false))) {
        reject(actorDataGliding ? "player_not_gliding" : "gliding_state_unavailable",
               actorDataGliding
                   ? "26.60 query.is_gliding returned false (ActorDataFlagComponent bit 32 is clear); no Space keymap request sent."
                   : "26.60 query.is_gliding helper could not be resolved and verified; no Space keymap request sent.",
               actorDataGliding ? 120 : 250);
        return;
    }
    const char* travelTagState = !glidingTravelTag ? "unavailable"
        : (*glidingTravelTag ? "present" : "absent");
    report("gliding_detected",
           std::string("26.60 query.is_gliding=true via ActorDataFlagComponent hash=0xC67426F3 bit=32; GlidingTravelFlagComponent=") +
               travelTagState + "; target_source=fresh_crosshair_player_query");
    const bool crosshairConfirmed = true; // The query returns only ray-intersecting players.

    // A new target starts Tracking. Range/height gates keep tracking active
    // until the activation conditions pass, without sending Space prematurely.
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_latchedTarget != target) {
            m_latchedTarget = target;
            m_targetLatched = false;
            m_burstRemaining = 0;
            m_triggerState = TriggerState::Tracking;
        }
        if (m_targetLatched &&
            bedaim::macefall::shouldRearmUnglide(geometry.distance, targetDistance)) {
            m_targetLatched = false;
            m_burstRemaining = 0;
            m_triggerState = TriggerState::Tracking;
        }
    }
    if (geometry.distance > effectiveMaximumDistance) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const bool preserveTriggeredLatch = bedaim::macefall::shouldKeepUnglideTriggerLatch(
                m_triggerState == TriggerState::Triggered && m_targetLatched,
                false, false, false);
            if (!preserveTriggeredLatch) {
                m_targetLatched = false;
                m_triggerState = TriggerState::Tracking;
            }
            m_burstRemaining = 0;
            m_nextAttempt = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
        }
        report("tracking_out_of_range",
            std::string("target_distance=") + std::to_string(geometry.distance) +
                ", activation_distance=" + std::to_string(effectiveMaximumDistance));
        return;
    }
    if (geometry.verticalDifference > maximumHeightDifference) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const bool preserveTriggeredLatch = bedaim::macefall::shouldKeepUnglideTriggerLatch(
                m_triggerState == TriggerState::Triggered && m_targetLatched,
                false, false, false);
            if (!preserveTriggeredLatch) {
                m_targetLatched = false;
                m_triggerState = TriggerState::Tracking;
            }
            m_burstRemaining = 0;
            m_nextAttempt = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
        }
        report("tracking_height_out_of_range",
            std::string("vertical_difference=") + std::to_string(geometry.verticalDifference) +
                ", activation_height=" + std::to_string(maximumHeightDifference));
        return;
    }
    if (!crosshairConfirmed) {
        reject("crosshair_not_on_target",
               "Camera ray does not intersect the selected player's hitbox; no Space keymap request sent.", 80);
        return;
    }

    const auto dispatchStarted = std::chrono::steady_clock::now();
    AutomationBroadcastResult result = AutomationBroadcastResult::BroadcastFailed;
    int remaining = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!enabled || !m_unglideEnabled || m_dispatchInProgress ||
            dispatchStarted < m_nextAttempt) {
            return;
        }
        if (m_triggerState != TriggerState::Triggered) {
            m_triggerState = TriggerState::Triggered;
            m_targetLatched = true;
            m_latchedTarget = target;
            m_burstRemaining = std::clamp(m_burstCount, 1, 10);
        }
        if (!bedaim::macefall::shouldBeginUnglideDispatch(
                enabled && m_unglideEnabled, m_dispatchInProgress,
                m_triggerState == TriggerState::Triggered, m_burstRemaining)) {
            m_nextAttempt = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
            return;
        }
        // Reserve one dispatch while locked, then release the mutex before JNI
        // and sendBroadcast so slow Java work cannot block config/disable callbacks.
        m_dispatchInProgress = true;
    }

    try {
        result = dispatchUnglideKeyMapperKeymap();
    } catch (...) {
        result = AutomationBroadcastResult::BroadcastFailed;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_dispatchInProgress = false;
        // Disable/re-enable may reset the state while the broadcast is in flight.
        // Do not let a late completion overwrite that newer state.
        if (m_latchedTarget == target &&
            m_triggerState == TriggerState::Triggered) {
            const bool sent = result == AutomationBroadcastResult::Sent;
            if (sent) --m_burstRemaining;
            else m_burstRemaining = 0;
            remaining = m_burstRemaining;
            m_nextAttempt = std::chrono::steady_clock::now() + std::chrono::milliseconds(
                sent ? (remaining > 0 ? std::clamp(m_pressIntervalMs, 50, 1000) : 160) : 500);
        } else {
            remaining = m_burstRemaining;
        }
    }

    report(automationBroadcastResultName(result),
        std::string("state=triggered; crosshair_confirmed=true; query_is_gliding=") +
            (actorDataGliding ? (*actorDataGliding ? "true" : "false") : "unavailable") +
            "; gliding_travel_tag=" + travelTagState +
            "; target_distance=" + std::to_string(geometry.distance) +
            ", vertical_difference=" + std::to_string(geometry.verticalDifference) +
            ", burst_remaining=" + std::to_string(remaining) +
            ", keymap_uid=" + kUnglideKeymapUid);
}
void UnglideModule::loadConfig(const nlohmann::json& j){Module::loadConfig(j);std::lock_guard<std::mutex> l(m_stateMutex);m_unglideEnabled=j.value("m_unglideEnabled",m_unglideEnabled);m_targetDistance=std::clamp(j.value("m_unglideTargetDistance",m_targetDistance),1.0f,32.0f);m_maxVerticalDifference=std::clamp(j.value("m_unglideMaxVerticalDifference",m_maxVerticalDifference),0.5f,16.0f);m_burstCount=std::clamp(j.value("m_unglideBurstCount",m_burstCount),1,10);m_pressIntervalMs=std::clamp(j.value("m_unglidePressIntervalMs",m_pressIntervalMs),50,1000);}
void UnglideModule::saveConfig(nlohmann::json& j){Module::saveConfig(j);std::lock_guard<std::mutex> l(m_stateMutex);j["m_unglideEnabled"]=m_unglideEnabled;j["m_unglideTargetDistance"]=m_targetDistance;j["m_unglideMaxVerticalDifference"]=m_maxVerticalDifference;j["m_unglideBurstCount"]=m_burstCount;j["m_unglidePressIntervalMs"]=m_pressIntervalMs;}
void UnglideModule::onMenuRegistered(){
 using namespace pl::modmenu;
 // Launcher-owned fields provide a persistent edit path; the compact slider
 // schema below remains available in the Unglide module page.
 static std::atomic<bool> persistentSettingsRegistered{false};
 bool expected = false;
 if (persistentSettingsRegistered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
   nlohmann::json persistentConfig;
   saveConfig(persistentConfig);
   auto persistentValue = [&](const char* key, const char* fallback) {
     const auto it = persistentConfig.find(key);
   