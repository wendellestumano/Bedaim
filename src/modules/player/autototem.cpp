#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "core/memory/MinecraftBuildIdentity.hpp"

#include "autototem.hpp"
#include "AutoTotemPolicy.hpp"
#include "aimassist.hpp"
#include "../ModuleRegistry.hpp"
#include "../../core/Diagnostics.hpp"
#include "../../core/InventoryAccess.hpp"
#include "../../core/JavaVm.hpp"
#include "../../config/ConfigManager.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/events/ScreenStateEvent.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <jni.h>
#include <string>
#include <string_view>
#include <utility>

namespace {

bool containsTotem(std::string_view identifier) {
    return identifier.find("totem") != std::string_view::npos;
}

std::string trim(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return std::string(value.substr(first, last - first));
}

bool validKeymapUid(std::string_view uid) noexcept {
    if (uid.size() != 36u) return false;
    for (std::size_t i = 0; i < uid.size(); ++i) {
        if (i == 8u || i == 13u || i == 18u || i == 23u) {
            if (uid[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(uid[i]))) {
            return false;
        }
    }
    return true;
}

bool parseBool(std::string_view value, bool& result) noexcept {
    if (value == "true" || value == "1" || value == "on" || value == "yes") {
        result = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "off" || value == "no") {
        result = false;
        return true;
    }
    return false;
}

using JniGetCreatedJavaVMsFn = jint (*)(JavaVM**, jsize, jsize*);

enum class VmLookupStatus { Ready, SymbolUnavailable, QueryFailed, NoVmFound, AttachFailed };

JniGetCreatedJavaVMsFn resolveJniGetCreatedJavaVMs() {
    static void* artHandle = dlopen("libart.so", RTLD_NOW);
    static void* helperHandle = dlopen("libnativehelper.so", RTLD_NOW);
    auto fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(
        dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!fn && artHandle)
        fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(dlsym(artHandle, "JNI_GetCreatedJavaVMs"));
    if (!fn && helperHandle)
        fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(dlsym(helperHandle, "JNI_GetCreatedJavaVMs"));
    return fn;
}

class KeyMapperJniEnvironment {
public:
    KeyMapperJniEnvironment() {
        VmLookupStatus lookupFailure = VmLookupStatus::SymbolUnavailable;
        const auto getVms = resolveJniGetCreatedJavaVMs();
        if (getVms) {
            JavaVM* vm = nullptr;
            jsize count = 0;
            const jint query = getVms(&vm, 1, &count);
            if (query == JNI_OK && count > 0 && vm) m_vm = vm;
            else if (query != JNI_OK) lookupFailure = VmLookupStatus::QueryFailed;
            else lookupFailure = VmLookupStatus::NoVmFound;
        }
        if (!m_vm) m_vm = bedrocktools::core::javaVm();
        if (!m_vm) { m_status = lookupFailure; return; }

        const jint envStatus = m_vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6);
        if (envStatus == JNI_OK) { m_status = VmLookupStatus::Ready; return; }
        if (envStatus != JNI_EDETACHED) { m_status = VmLookupStatus::AttachFailed; return; }
        if (m_vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK) {
            m_attached = true;
            m_status = VmLookupStatus::Ready;
        } else {
            m_env = nullptr;
            m_status = VmLookupStatus::AttachFailed;
        }
    }

    ~KeyMapperJniEnvironment() {
        if (m_attached && m_vm) m_vm->DetachCurrentThread();
    }

    JNIEnv* get() const noexcept { return m_env; }
    VmLookupStatus status() const noexcept { return m_status; }

private:
    JavaVM* m_vm = nullptr;
    JNIEnv* m_env = nullptr;
    bool m_attached = false;
    VmLookupStatus m_status = VmLookupStatus::SymbolUnavailable;
};

bool clearJniException(JNIEnv* env) {
    if (!env || !env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

enum class KeyMapperDispatchResult {
    Sent, InvalidUid, VmSymbolUnavailable, VmQueryFailed, VmNotFound,
    ThreadAttachFailed, ContextUnavailable, IntentUnavailable, BroadcastFailed
};

KeyMapperDispatchResult dispatchKeyMapperKeymap(std::string_view keymapUid) {
    if (!validKeymapUid(keymapUid)) return KeyMapperDispatchResult::InvalidUid;

    KeyMapperJniEnvironment jni;
    JNIEnv* env = jni.get();
    if (!env) {
        switch (jni.status()) {
            case VmLookupStatus::SymbolUnavailable: return KeyMapperDispatchResult::VmSymbolUnavailable;
            case VmLookupStatus::QueryFailed: return KeyMapperDispatchResult::VmQueryFailed;
            case VmLookupStatus::NoVmFound: return KeyMapperDispatchResult::VmNotFound;
            case VmLookupStatus::AttachFailed: return KeyMapperDispatchResult::ThreadAttachFailed;
            case VmLookupStatus::Ready: break;
        }
        return KeyMapperDispatchResult::VmSymbolUnavailable;
    }

    if (env->PushLocalFrame(40) != JNI_OK) {
        clearJniException(env);
        return KeyMapperDispatchResult::IntentUnavailable;
    }
    struct LocalFrameGuard {
        JNIEnv* env;
        ~LocalFrameGuard() { env->PopLocalFrame(nullptr); }
    } frame{env};

    jobject app = nullptr;
    jclass threadClass = env->FindClass("android/app/ActivityThread");
    if (threadClass && !clearJniException(env)) {
        const jmethodID currentApplication = env->GetStaticMethodID(
            threadClass, "currentApplication", "()Landroid/app/Application;");
        if (currentApplication && !clearJniException(env)) {
            app = env->CallStaticObjectMethod(threadClass, currentApplication);
            if (clearJniException(env)) app = nullptr;
        } else clearJniException(env);
    } else clearJniException(env);

    if (!app) {
        jclass globals = env->FindClass("android/app/AppGlobals");
        if (globals && !clearJniException(env)) {
            const jmethodID initialApplication = env->GetStaticMethodID(
                globals, "getInitialApplication", "()Landroid/app/Application;");
            if (initialApplication && !clearJniException(env)) {
                app = env->CallStaticObjectMethod(globals, initialApplication);
                if (clearJniException(env)) app = nullptr;
            } else clearJniException(env);
        } else clearJniException(env);
    }
    if (!app) return KeyMapperDispatchResult::ContextUnavailable;

    jclass intentClass = env->FindClass("android/content/Intent");
    if (!intentClass || clearJniException(env)) return KeyMapperDispatchResult::IntentUnavailable;
    const jmethodID intentCtor = env->GetMethodID(intentClass, "<init>", "()V");
    const jmethodID setAction = env->GetMethodID(
        intentClass, "setAction", "(Ljava/lang/String;)Landroid/content/Intent;");
    const jmethodID setComponent = env->GetMethodID(
        intentClass, "setComponent", "(Landroid/content/ComponentName;)Landroid/content/Intent;");
    const jmethodID putExtra = env->GetMethodID(
        intentClass, "putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;");
    if (clearJniException(env) || !intentCtor || !setAction || !setComponent || !putExtra)
        return KeyMapperDispatchResult::IntentUnavailable;

    jclass componentClass = env->FindClass("android/content/ComponentName");
    if (!componentClass || clearJniException(env)) return KeyMapperDispatchResult::IntentUnavailable;
    const jmethodID componentCtor = env->GetMethodID(
        componentClass, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (!componentCtor || clearJniException(env)) return KeyMapperDispatchResult::IntentUnavailable;

    jobject intent = env->NewObject(intentClass, intentCtor);
    if (!intent || clearJniException(env)) return KeyMapperDispatchResult::IntentUnavailable;

    constexpr const char* packageName = "io.github.sds100.keymapper";
    constexpr const char* receiverName =
        "io.github.sds100.keymapper.api.TriggerKeyMapsBroadcastReceiver";
    constexpr const char* actionName =
        "io.github.sds100.keymapper.ACTION_TRIGGER_KEYMAP_BY_UID";
    constexpr const char* uidExtraName =
        "io.github.sds100.keymapper.EXTRA_KEYMAP_UID";

    const std::string uidOwned(keymapUid);
    jstring action = env->NewStringUTF(actionName);
    jstring package = env->NewStringUTF(packageName);
    jstring receiver = env->NewStringUTF(receiverName);
    jstring extra = env->NewStringUTF(uidExtraName);
    jstring uid = env->NewStringUTF(uidOwned.c_str());
    if (!action || !package || !receiver || !extra || !uid || clearJniException(env))
        return KeyMapperDispatchResult::IntentUnavailable;

    jobject component = env->NewObject(componentClass, componentCtor, package, receiver);
    if (!component || clearJniException(env)) return KeyMapperDispatchResult::IntentUnavailable;

    env->CallObjectMethod(intent, setAction, action);
    if (clearJniException(env)) return KeyMapperDispatchResult::IntentUnavailable;
    env->CallObjectMethod(intent, setComponent, component);
    if (clearJniException(env)) return KeyMapperDispatchResult::IntentUnavailable;
    env->CallObjectMethod(intent, putExtra, extra, uid);
    if (clearJniException(env)) return KeyMapperDispatchResult::IntentUnavailable;

    jclass contextClass = env->FindClass("android/content/Context");
    if (!contextClass || clearJniException(env)) return KeyMapperDispatchResult::IntentUnavailable;
    const jmethodID sendBroadcast = env->GetMethodID(
        contextClass, "sendBroadcast", "(Landroid/content/Intent;)V");
    if (!sendBroadcast || clearJniException(env)) return KeyMapperDispatchResult::IntentUnavailable;

    env->CallVoidMethod(app, sendBroadcast, intent);
    if (clearJniException(env)) return KeyMapperDispatchResult::BroadcastFailed;
    return KeyMapperDispatchResult::Sent;
}

const char* dispatchResultName(KeyMapperDispatchResult result) noexcept {
    switch (result) {
        case KeyMapperDispatchResult::Sent: return "keymapper_broadcast_sent";
        case KeyMapperDispatchResult::InvalidUid: return "invalid_keymap_uid";
        case KeyMapperDispatchResult::VmSymbolUnavailable: return "android_jni_vm_symbol_unavailable";
        case KeyMapperDispatchResult::VmQueryFailed: return "android_jni_vm_query_failed";
        case KeyMapperDispatchResult::VmNotFound: return "android_jni_vm_not_found";
        case KeyMapperDispatchResult::ThreadAttachFailed: return "android_jni_thread_attach_failed";
        case KeyMapperDispatchResult::ContextUnavailable: return "android_context_unavailable";
        case KeyMapperDispatchResult::IntentUnavailable: return "keymapper_intent_api_unavailable";
        case KeyMapperDispatchResult::BroadcastFailed: return "keymapper_broadcast_failed";
    }
    return "keymapper_broadcast_unknown";
}

std::atomic_bool g_loggedBaselineMismatch{false};
std::atomic_bool g_loggedOffhandReadFailure{false};
std::atomic_bool g_loggedMissingKeymapUid{false};
std::atomic_bool g_loggedRetryExhausted{false};

} // namespace

AutoTotemModule::AutoTotemModule()
    : Module("Auto Totem",
             "Triggers a configured Key Mapper keymap when the offhand is empty and a totem is available.") {
    keybind = 0;
}

void AutoTotemModule::onInit() {
    const bool exact260 = bedrocktools::core::memory::isExactMinecraft260Build();
    bedrocktools::diagnostics::probe(
        exact260 ? bedrocktools::diagnostics::Level::Info : bedrocktools::diagnostics::Level::Warning,
        "AutoTotem", "native_baseline", exact260 ? "verified" : "unsupported",
        "read-only inventory access requires libminecraftpe.so Build ID eb291b883dfdfaff70c4992c9eabd61885ae3e15");
    if (exact260) bedrocktools::core::InventoryAccess::get().initialize();

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { processPlayerTick(event.player); });
    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>(
        [this](auto& event) {
            if (event.screen != bedrocktools::events::ScreenKind::Container) return;
            m_containerOpen.store(event.phase == bedrocktools::events::ScreenPhase::Opened,
                                  std::memory_order_release);
        });
}

void AutoTotemModule::onEnable() {
    if (bedrocktools::core::memory::isExactMinecraft260Build())
        bedrocktools::core::InventoryAccess::get().initialize();
    std::lock_guard lock(m_stateMutex);
    m_pendingRefill = false;
    m_lastOffhandHadTotem = false;
    m_waitingForKeymap = false;
    m_dispatchInProgress = false;
    m_retryBlocked = false;
    m_confirmationPending = false;
    m_dispatchAttempts = 0;
    m_refillAfter = {};
    m_nextAttempt = {};
    m_confirmationDeadline = {};
}

void AutoTotemModule::onDisable() {
    std::lock_guard lock(m_stateMutex);
    m_pendingRefill = false;
    m_lastOffhandHadTotem = false;
    m_waitingForKeymap = false;
    m_dispatchInProgress = false;
    m_retryBlocked = false;
    m_confirmationPending = false;
    m_dispatchAttempts = 0;
    m_refillAfter = {};
    m_nextAttempt = {};
    m_confirmationDeadline = {};
}

void AutoTotemModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    if (!enabled || !player) return;
    if (!bedrocktools::core::memory::isExactMinecraft260Build()) {
        if (!g_loggedBaselineMismatch.exchange(true, std::memory_order_acq_rel))
            bedrocktools::diagnostics::probe(
                bedrocktools::diagnostics::Level::Error, "AutoTotem", "native_baseline", "unsupported",
                "refusing to read inventory offsets because the exact Minecraft 26.60 ELF Build ID did not match");
        return;
    }

    auto& inventory = bedrocktools::core::InventoryAccess::get();
    std::string offhandId;
    std::string offhandError;
    bool offhandEmpty = false;
    if (!inventory.readOffhandState(player, offhandId, offhandEmpty, &offhandError)) {
        if (!g_loggedOffhandReadFailure.exchange(true, std::memory_order_acq_rel))
            bedrocktools::diagnostics::probe(
                bedrocktools::diagnostics::Level::Error, "AutoTotem", "offhand_state", "unreadable",
                offhandError + "; no Key Mapper request sent");
        return;
    }

    const bool offhandHasTotem = containsTotem(offhandId);
    const int totemSlot = inventory.findInventorySlot(player, "totem");

    bool inCombat = player->hurtTime() > 0;
    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (aimBase && aimBase->enabled) {
        auto* aim = static_cast<AimAssistModule*>(aimBase);
        void* target = nullptr;
        bedrocktools::sdk::AABB bounds{};
        float range = 0.0f;
        if (aim->currentTargetSnapshot(target, bounds, range)) inCombat = true;
    }

    const auto now = std::chrono::steady_clock::now();
    std::unique_lock lock(m_stateMutex);
    if (!enabled) return;

    if (offhandHasTotem) {
        m_lastOffhandHadTotem = true;
        m_pendingRefill = false;
        m_waitingForKeymap = false;
        m_dispatchAttempts = 0;
        m_retryBlocked = false;
        m_confirmationPending = false;
        m_refillAfter = {};
        m_nextAttempt = {};
        m_confirmationDeadline = {};
        return;
    }

    // Never overwrite a shield or any other non-empty offhand item.
    if (!offhandEmpty) {
        m_lastOffhandHadTotem = false;
        m_pendingRefill = false;
        m_waitingForKeymap = false;
        m_dispatchAttempts = 0;
        m_retryBlocked = false;
        m_confirmationPending = false;
        m_refillAfter = {};
        m_nextAttempt = {};
        m_confirmationDeadline = {};
        return;
    }

    if (m_lastOffhandHadTotem) {
        m_pendingRefill = true;
        m_refillAfter = now + std::chrono::milliseconds(
            static_cast<int>(bedaim::autototem::normalizeDelayMs(m_delayMs)));
        m_lastOffhandHadTotem = false;
        m_waitingForKeymap = false;
        m_dispatchAttempts = 0;
        m_retryBlocked = false;
        m_nextAttempt = {};
    } else if (m_keepTotemEquipped && !m_pendingRefill) {
        m_pendingRefill = true;
        m_refillAfter = now;
    }

    const bool confirmationExpired = bedaim::autototem::confirmationTimedOut(
        m_confirmationPending, offhandHasTotem,
        m_confirmationDeadline.time_since_epoch().count() != 0 && now >= m_confirmationDeadline);
    if (confirmationExpired) {
        m_confirmationPending = false;
        m_confirmationDeadline = {};
    }
    if (!m_pendingRefill || m_retryBlocked || m_dispatchInProgress) {
        lock.unlock();
        if (confirmationExpired) {
            bedrocktools::diagnostics::probe(
                bedrocktools::diagnostics::Level::Warning, "AutoTotem", "keymapper_confirmation",
                "timeout", "Android accepted the Key Mapper broadcast, but no totem appeared in the offhand within 1500 ms; automatic resend is suppressed to avoid duplicate swap actions. Toggle AutoTotem or change its UID to retry manually.");
        }
        return;
    }

    const bool delayElapsed = m_refillAfter.time_since_epoch().count() == 0 || now >= m_refillAfter;
    const bool containerOpen = m_containerOpen.load(std::memory_order_acquire);
    const bedaim::autototem::Inputs input{
        enabled, m_pendingRefill, offhandHasTotem, delayElapsed, m_combatOnly,
        inCombat, containerOpen, totemSlot >= 0, offhandEmpty
    };
    if (bedaim::autototem::decide(input) != bedaim::autototem::Decision::Equip) return;

    if (m_keymapUid.empty()) {
        if (!g_loggedMissingKeymapUid.exchange(true, std::memory_order_acq_rel))
            bedrocktools::diagnostics::probe(
                bedrocktools::diagnostics::Level::Warning, "AutoTotem", "keymapper_uid", "not_configured",
                "set the AutoTotem Key Mapper keymap UID in AutoTotem Settings; no other module UID is reused");
        return;
    }

    if (m_waitingForKeymap && now < m_nextAttempt) return;
    if (m_dispatchAttempts >= bedaim::autototem::kMaxDispatchAttempts) {
        m_retryBlocked = true;
        m_waitingForKeymap = false;
        if (!g_loggedRetryExhausted.exchange(true, std::memory_order_acq_rel))
            bedrocktools::diagnostics::probe(
                bedrocktools::diagnostics::Level::Error, "AutoTotem", "keymapper_retry", "exhausted",
                "three Key Mapper broadcasts were attempted without observing a totem in the offhand; toggle AutoTotem to retry");
        return;
    }

    const std::string uid = m_keymapUid;
    m_dispatchInProgress = true;
    lock.unlock();

    const auto result = dispatchKeyMapperKeymap(uid);
    const auto completedAt = std::chrono::steady_clock::now();

    lock.lock();
    m_dispatchInProgress = false;
    if (!enabled) return;
    ++m_dispatchAttempts;
    const int attemptNumber = m_dispatchAttempts;
    const bool retry = bedaim::autototem::shouldRetryDispatch(
        result == KeyMapperDispatchResult::Sent, attemptNumber);
    m_waitingForKeymap = retry;
    if (retry) {
        m_confirmationPending = false;
        m_confirmationDeadline = {};
        m_nextAttempt = completedAt + std::chrono::milliseconds(500);
    } else {
        // A successful broadcast may still be executing asynchronously. Wait
        // for the offhand state to confirm completion; never resend an accepted
        // keymap action automatically because a second inventory sequence could
        // undo the first. Failed broadcasts get at most two bounded retries.
        m_retryBlocked = true;
        m_waitingForKeymap = false;
        m_nextAttempt = {};
        m_confirmationPending = result == KeyMapperDispatchResult::Sent;
        m_confirmationDeadline = m_confirmationPending
            ? completedAt + std::chrono::milliseconds(1500)
            : std::chrono::steady_clock::time_point{};
    }
    lock.unlock();

    std::string dispatchDetail = "attempt=" + std::to_string(attemptNumber) +
        "; no native inventory setter was called";
    if (result == KeyMapperDispatchResult::Sent) {
        dispatchDetail += "; broadcast accepted; waiting for offhand confirmation; automatic resend suppressed to prevent duplicate actions";
    } else if (retry) {
        dispatchDetail += "; broadcast failed; bounded retry scheduled";
    } else {
        dispatchDetail += "; broadcast failed; retry budget exhausted";
    }
    bedrocktools::diagnostics::probe(
        result == KeyMapperDispatchResult::Sent ? bedrocktools::diagnostics::Level::Info
                                                : bedrocktools::diagnostics::Level::Warning,
        "AutoTotem", "keymapper_dispatch", dispatchResultName(result), dispatchDetail);
}

void AutoTotemModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard lock(m_stateMutex);
    m_delayMs = bedaim::autototem::normalizeDelayMs(json.value("m_delayMs", m_delayMs));
    m_combatOnly = json.value("m_combatOnly", m_combatOnly);
    m_keepTotemEquipped = json.value("m_keepTotemEquipped", m_keepTotemEquipped);
    const auto uid = json.value("m_keymapUid", m_keymapUid);
    m_keymapUid = uid.empty() || validKeymapUid(uid) ? uid : std::string{};
}

void AutoTotemModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard lock(m_stateMutex);
    json["m_delayMs"] = bedaim::autototem::normalizeDelayMs(m_delayMs);
    json["m_combatOnly"] = m_combatOnly;
    json["m_keepTotemEquipped"] = m_keepTotemEquipped;
    json["m_keymapUid"] = m_keymapUid;
}

bool AutoTotemModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "m_keymapUid") {
        const std::string uid = trim(value);
        if (!uid.empty() && !validKeymapUid(uid)) return false;
        std::lock_guard lock(m_stateMutex);
        m_keymapUid = uid;
        m_retryBlocked = false;
        m_confirmationPending = false;
        m_dispatchAttempts = 0;
        m_waitingForKeymap = false;
        m_nextAttempt = {};
        m_confirmationDeadline = {};
        g_loggedMissingKeymapUid.store(false, std::memory_order_release);
        g_loggedRetryExhausted.store(false, std::memory_order_release);
        return true;
    }

    if (key == "m_combatOnly" || key == "m_keepTotemEquipped") {
        bool enabledValue = false;
        if (!parseBool(value, enabledValue)) return false;
        std::lock_guard lock(m_stateMutex);
        if (key == "m_combatOnly") m_combatOnly = enabledValue;
        else m_keepTotemEquipped = enabledValue;
        return true;
    }

    if (key == "m_delayMs") {
        try {
            const std::string raw(value);
            std::size_t parsed = 0;
            const float number = std::stof(raw, &parsed);
            while (parsed < raw.size() && std::isspace(static_cast<unsigned char>(raw[parsed]))) ++parsed;
            if (parsed != raw.size() || !std::isfinite(number)) return false;
            std::lock_guard lock(m_stateMutex);
            m_delayMs = bedaim::autototem::normalizeDelayMs(number);
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

void AutoTotemModule::onMenuRegistered() {
    using namespace pl::modmenu;
    static std::atomic_bool settingsRegistered{false};
    if (!settingsRegistered.exchange(true, std::memory_order_acq_rel)) {
        ModuleBuilder settings("bedrocktools.autototem.persistent_settings", "AutoTotem Settings");
        settings.modId("Bedaim")
            .description("Configure the Key Mapper keymap UID used for offhand refill.")
            .defaultEnabled(true)
            .config("m_keymapUid", "Key Mapper keymap UID", ConfigType::Text, m_keymapUid)
            .onConfigChanged([this](std::string_view, std::string_view key, std::string_view value) {
                if (onMenuConfigChanged(key, value)) bedrocktools::config::ConfigManager::get().save();
            });
        if (!settings.registerModule())
            settingsRegistered.store(false, std::memory_order_release);
    }

    ConfigSchemaBuilder schema;
    schema.defaultCategory("behavior")
        .category("behavior", "Behavior", "Automatic offhand refill")
        .category("safety", "Safety", "Conditions required before a Key Mapper request");

    auto makeNode = [](const char* key, const char* title, const char* category,
                       ConfigControlTypeV2 type) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.title = title;
        node.category = category;
        node.type = type;
        return node;
    };

    auto delay = makeNode("m_delayMs", "Refill Delay (ms)", "behavior",
                          ConfigControlTypeV2::SliderFloat);
    delay.defaultValue = "100";
    delay.currentValue = "100";
    delay.hasCurrentValue = true;
    delay.minValue = "0";
    delay.maxValue = "1000";
    delay.step = "10";
    schema.node(std::move(delay));

    auto keep = makeNode("m_keepTotemEquipped", "Keep Totem Equipped", "behavior",
                         ConfigControlTypeV2::Toggle);
    keep.defaultValue = "true";
    keep.currentValue = "true";
    keep.hasCurrentValue = true;
    schema.node(std::move(keep));

    auto combat = makeNode("m_combatOnly", "Combat Only", "safety",
                           ConfigControlTypeV2::Toggle);
    combat.defaultValue = "false";
    combat.currentValue = "false";
    combat.hasCurrentValue = true;
    schema.node(std::move(combat));

    schema.node(makeNode("keybind", "Keybind", "behavior", ConfigControlTypeV2::Keybind));
    setConfigSchemaJson(moduleId, schema.toJson());
}
