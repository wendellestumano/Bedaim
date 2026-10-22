#include "autohit.hpp"

#include "../../core/Diagnostics.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/events/ScreenStateEvent.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <dlfcn.h>
#include <jni.h>
#include <string>
#include <utility>

namespace {
constexpr const char* kMacroDroidPackage = "com.arlosoft.macrodroid";
constexpr const char* kAutoHitKeyZeroAction =
    "com.wendellestumano.bedaim.AUTOHIT_KEY0";

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

class AutoHitJniEnvironment {
public:
    AutoHitJniEnvironment() {
        const auto getVms = resolveJniGetCreatedJavaVMs();
        if (!getVms) return;

        JavaVM* vm = nullptr;
        jsize count = 0;
        if (getVms(&vm, 1, &count) != JNI_OK || count < 1 || !vm) return;

        m_vm = vm;
        const jint status = m_vm->GetEnv(
            reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6);
        if (status == JNI_OK) return;
        if (status != JNI_EDETACHED) {
            m_env = nullptr;
            return;
        }

        if (m_vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK) {
            m_attached = true;
        } else {
            m_env = nullptr;
        }
    }

    ~AutoHitJniEnvironment() {
        if (m_attached && m_vm) m_vm->DetachCurrentThread();
    }

    JNIEnv* get() const noexcept { return m_env; }

private:
    JavaVM* m_vm = nullptr;
    JNIEnv* m_env = nullptr;
    bool m_attached = false;
};

bool clearJniException(JNIEnv* env) {
    if (!env || !env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

enum class DispatchResult {
    Sent,
    VmUnavailable,
    ContextUnavailable,
    IntentUnavailable,
    BroadcastFailed
};

DispatchResult sendKeyZeroIntentToMacroDroid() {
    AutoHitJniEnvironment jni;
    JNIEnv* env = jni.get();
    if (!env) return DispatchResult::VmUnavailable;

    if (env->PushLocalFrame(32) != JNI_OK) {
        clearJniException(env);
        return DispatchResult::IntentUnavailable;
    }
    struct LocalFrameGuard {
        JNIEnv* env;
        ~LocalFrameGuard() { env->PopLocalFrame(nullptr); }
    } frame{env};

    jobject app = nullptr;
    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if (activityThread && !clearJniException(env)) {
        const jmethodID currentApplication = env->GetStaticMethodID(
            activityThread, "currentApplication", "()Landroid/app/Application;");
        if (currentApplication && !clearJniException(env)) {
            app = env->CallStaticObjectMethod(activityThread, currentApplication);
            if (clearJniException(env)) app = nullptr;
        } else {
            clearJniException(env);
        }
    } else {
        clearJniException(env);
    }

    if (!app) {
        jclass appGlobals = env->FindClass("android/app/AppGlobals");
        if (appGlobals && !clearJniException(env)) {
            const jmethodID initialApplication = env->GetStaticMethodID(
                appGlobals, "getInitialApplication", "()Landroid/app/Application;");
            if (initialApplication && !clearJniException(env)) {
                app = env->CallStaticObjectMethod(appGlobals, initialApplication);
                if (clearJniException(env)) app = nullptr;
            } else {
                clearJniException(env);
            }
        } else {
            clearJniException(env);
        }
    }
    if (!app) return DispatchResult::ContextUnavailable;

    jclass intentClass = env->FindClass("android/content/Intent");
    if (!intentClass || clearJniException(env)) return DispatchResult::IntentUnavailable;

    const jmethodID constructor = env->GetMethodID(intentClass, "<init>", "()V");
    const jmethodID setAction = env->GetMethodID(
        intentClass, "setAction", "(Ljava/lang/String;)Landroid/content/Intent;");
    const jmethodID setPackage = env->GetMethodID(
        intentClass, "setPackage", "(Ljava/lang/String;)Landroid/content/Intent;");
    if (clearJniException(env) || !constructor || !setAction || !setPackage) {
        return DispatchResult::IntentUnavailable;
    }

    jobject intent = env->NewObject(intentClass, constructor);
    if (!intent || clearJniException(env)) return DispatchResult::IntentUnavailable;

    jstring action = env->NewStringUTF(kAutoHitKeyZeroAction);
    jstring packageName = env->NewStringUTF(kMacroDroidPackage);
    if (!action || !packageName || clearJniException(env)) {
        return DispatchResult::IntentUnavailable;
    }

    env->CallObjectMethod(intent, setAction, action);
    if (clearJniException(env)) return DispatchResult::IntentUnavailable;
    env->CallObjectMethod(intent, setPackage, packageName);
    if (clearJniException(env)) return DispatchResult::IntentUnavailable;

    jclass contextClass = env->FindClass("android/content/Context");
    if (!contextClass || clearJniException(env)) return DispatchResult::IntentUnavailable;

    const jmethodID sendBroadcast = env->GetMethodID(
        contextClass, "sendBroadcast", "(Landroid/content/Intent;)V");
    if (!sendBroadcast || clearJniException(env)) return DispatchResult::IntentUnavailable;

    env->CallVoidMethod(app, sendBroadcast, intent);
    if (clearJniException(env)) return DispatchResult::BroadcastFailed;
    return DispatchResult::Sent;
}

const char* dispatchResultName(DispatchResult result) {
    switch (result) {
        case DispatchResult::Sent: return "macrodroid_key0_broadcast_sent";
        case DispatchResult::VmUnavailable: return "android_java_vm_unavailable";
        case DispatchResult::ContextUnavailable: return "android_context_unavailable";
        case DispatchResult::IntentUnavailable: return "android_intent_api_unavailable";
        case DispatchResult::BroadcastFailed: return "macrodroid_key0_broadcast_failed";
    }
    return "macrodroid_key0_broadcast_unknown";
}
} // namespace

using bedrocktools::sdk::Player;

AutoHitModule::AutoHitModule()
    : Module("Auto Hit",
             "Sends the configured MacroDroid key-0 trigger at a limited rate.") {
    keybind = 0;
}

void AutoHitModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { processPlayerTick(event.player); });

    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>(
        [this](auto& event) {
            const bool opened = event.phase == bedrocktools::events::ScreenPhase::Opened;
            if (event.screen == bedrocktools::events::ScreenKind::Container) {
                m_containerOpen.store(opened, std::memory_order_release);
            } else if (event.screen == bedrocktools::events::ScreenKind::Chat) {
                m_chatOpen.store(opened, std::memory_order_release);
            }
        });

    bedrocktools::diagnostics::probe(
        bedrocktools::diagnostics::Level::Info, "AutoHit", "lifecycle", "initialized",
        "simple MacroDroid key-0 dispatcher; no native attack or Aim Assist dependency");
}

void AutoHitModule::onEnable() {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_nextDispatch = {};
        m_loggedFirstDispatch = false;
    }
    bedrocktools::diagnostics::probe(
        bedrocktools::diagnostics::Level::Info, "AutoHit", "lifecycle", "enabled",
        "will send Intent com.wendellestumano.bedaim.AUTOHIT_KEY0 to MacroDroid");
}

void AutoHitModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_nextDispatch = {};
    bedrocktools::diagnostics::probe(
        bedrocktools::diagnostics::Level::Info, "AutoHit", "lifecycle", "disabled");
}

bool AutoHitModule::onMouseEvent(int, bool) {
    return false; // Never consume or alter normal mouse/touch input.
}

void AutoHitModule::processPlayerTick(Player* player) {
    using namespace bedrocktools;

    if (!enabled) return;
    if (!player) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "dispatch_gate", "missing_player");
        return;
    }
    if (m_containerOpen.load(std::memory_order_acquire) ||
        m_chatOpen.load(std::memory_order_acquire)) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::chrono::milliseconds interval{125};
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        const float cps = std::clamp(m_cps, 1.0f, 20.0f);
        interval = std::chrono::milliseconds(
            static_cast<int>(std::lround(1000.0f / cps)));
        if (m_nextDispatch.time_since_epoch().count() != 0 && now < m_nextDispatch) {
            return;
        }
        // Set the next time before JNI dispatch so a slow/erroring call cannot
        // trigger a burst of retries on subsequent game ticks.
        m_nextDispatch = now + interval;
    }

    diagnostics::probe(
        diagnostics::Level::Debug, "AutoHit", "key0_dispatch", "attempt",
        "sending package-scoped Intent to com.arlosoft.macrodroid; action=" +
            std::string(kAutoHitKeyZeroAction));

    const DispatchResult result = sendKeyZeroIntentToMacroDroid();
    const bool sent = result == DispatchResult::Sent;
    bool firstSuccessfulDispatch = false;
    if (sent) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        firstSuccessfulDispatch = !m_loggedFirstDispatch;
        m_loggedFirstDispatch = true;
    }
    diagnostics::probe(
        !sent ? diagnostics::Level::Warning :
            (firstSuccessfulDispatch ? diagnostics::Level::Info : diagnostics::Level::Debug),
        "AutoHit", "key0_dispatch", dispatchResultName(result),
        sent
            ? "Android accepted the broadcast. This does not confirm MacroDroid ran or Minecraft received key 0."
            : "The broadcast failed before key delivery; inspect the Android VM/context/Intent result.");
}

void AutoHitModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_cps = std::clamp(json.value("m_cps", m_cps), 1.0f, 20.0f);
}

void AutoHitModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    json["m_cps"] = m_cps;
}

void AutoHitModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("combat")
        .category("combat", "Combat", "MacroDroid keyboard trigger");

    ConfigNodeV2 cpsNode;
    cpsNode.id = "m_cps";
    cpsNode.key = "m_cps";
    cpsNode.title = "MacroDroid trigger rate";
    cpsNode.category = "combat";
    cpsNode.type = ConfigControlTypeV2::SliderFloat;
    cpsNode.minValue = "1";
    cpsNode.maxValue = "20";
    cpsNode.step = "1";
    cpsNode.unit = " CPS";
    schema.node(std::move(cpsNode));

    ConfigNodeV2 keybindNode;
    keybindNode.id = "keybind";
    keybindNode.key = "keybind";
    keybindNode.title = "Keybind";
    keybindNode.category = "combat";
    keybindNode.type = ConfigControlTypeV2::Keybind;
    schema.node(std::move(keybindNode));

    setConfigSchemaJson(moduleId, schema.toJson());
}

bool AutoHitModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "keybind") return false;
    if (key != "m_cps") return false;

    try {
        const std::string raw(value);
        std::size_t parsed = 0;
        const float number = std::stof(raw, &parsed);
        while (parsed < raw.size() &&
               std::isspace(static_cast<unsigned char>(raw[parsed]))) {
            ++parsed;
        }
        if (parsed != raw.size() || !std::isfinite(number)) return false;

        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_cps = std::clamp(number, 1.0f, 20.0f);
        bedrocktools::diagnostics::probe(
            bedrocktools::diagnostics::Level::Info, "AutoHit", "config", "cps_updated",
            "MacroDroid trigger rate=" + std::to_string(m_cps));
        return true;
    } catch (...) {
        return false;
    }
}
