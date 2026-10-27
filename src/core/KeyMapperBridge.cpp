#include "KeyMapperBridge.hpp"
#include "JavaVm.hpp"

#include <dlfcn.h>
#include <jni.h>

#include <string>

namespace bedrocktools::core {
namespace {
constexpr const char* kPackage = "io.github.sds100.keymapper";
constexpr const char* kReceiver =
    "io.github.sds100.keymapper.api.TriggerKeyMapsBroadcastReceiver";
constexpr const char* kAction = "io.github.sds100.keymapper.ACTION_TRIGGER_KEYMAP_BY_UID";
constexpr const char* kUidExtra = "io.github.sds100.keymapper.EXTRA_KEYMAP_UID";

using GetCreatedJavaVMsFn = jint (*)(JavaVM**, jsize, jsize*);

GetCreatedJavaVMsFn resolveGetCreatedJavaVMs() noexcept {
    static void* art = dlopen("libart.so", RTLD_NOW);
    static void* helper = dlopen("libnativehelper.so", RTLD_NOW);
    auto fn = reinterpret_cast<GetCreatedJavaVMsFn>(
        dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!fn && art) fn = reinterpret_cast<GetCreatedJavaVMsFn>(
        dlsym(art, "JNI_GetCreatedJavaVMs"));
    if (!fn && helper) fn = reinterpret_cast<GetCreatedJavaVMsFn>(
        dlsym(helper, "JNI_GetCreatedJavaVMs"));
    return fn;
}

class JniEnvironment {
public:
    JniEnvironment() noexcept {
        if (const auto getVms = resolveGetCreatedJavaVMs()) {
            JavaVM* vm = nullptr;
            jsize count = 0;
            if (getVms(&vm, 1, &count) == JNI_OK && count > 0 && vm) m_vm = vm;
        }
        if (!m_vm) m_vm = javaVm();
        if (!m_vm) return;

        const jint state = m_vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6);
        if (state == JNI_OK) return;
        if (state != JNI_EDETACHED) {
            m_env = nullptr;
            m_attachFailed = true;
            return;
        }
        if (m_vm->AttachCurrentThread(&m_env, nullptr) == JNI_OK) m_attached = true;
        else {
            m_env = nullptr;
            m_attachFailed = true;
        }
    }

    ~JniEnvironment() {
        if (m_attached && m_vm) m_vm->DetachCurrentThread();
    }

    JNIEnv* get() const noexcept { return m_env; }
    bool attachFailed() const noexcept { return m_attachFailed; }

private:
    JavaVM* m_vm = nullptr;
    JNIEnv* m_env = nullptr;
    bool m_attached = false;
    bool m_attachFailed = false;
};

bool clearException(JNIEnv* env) noexcept {
    if (!env || !env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

jobject currentApplication(JNIEnv* env) noexcept {
    jobject app = nullptr;
    jclass thread = env->FindClass("android/app/ActivityThread");
    if (thread && !clearException(env)) {
        const jmethodID method = env->GetStaticMethodID(
            thread, "currentApplication", "()Landroid/app/Application;");
        if (method && !clearException(env)) {
            app = env->CallStaticObjectMethod(thread, method);
            if (clearException(env)) app = nullptr;
        } else {
            clearException(env);
        }
    } else {
        clearException(env);
    }

    if (app) return app;
    jclass globals = env->FindClass("android/app/AppGlobals");
    if (globals && !clearException(env)) {
        const jmethodID method = env->GetStaticMethodID(
            globals, "getInitialApplication", "()Landroid/app/Application;");
        if (method && !clearException(env)) {
            app = env->CallStaticObjectMethod(globals, method);
            if (clearException(env)) app = nullptr;
        } else {
            clearException(env);
        }
    } else {
        clearException(env);
    }
    return app;
}

// Accept UUID-like input only. This prevents accidental arbitrary strings from
// being passed into a broadcast intended for the user's own Key Mapper setup.
bool validUid(std::string_view uid) noexcept {
    if (uid.size() != 36) return false;
    for (std::size_t i = 0; i < uid.size(); ++i) {
        const char c = uid[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}
} // namespace

KeyMapperDispatchResult dispatchKeyMapperKeymap(std::string_view uid) noexcept {
    if (!validUid(uid)) return KeyMapperDispatchResult::InvalidUid;

    JniEnvironment jni;
    JNIEnv* env = jni.get();
    if (!env) return jni.attachFailed()
        ? KeyMapperDispatchResult::ThreadAttachFailed
        : KeyMapperDispatchResult::VmUnavailable;
    if (env->PushLocalFrame(32) != JNI_OK) {
        clearException(env);
        return KeyMapperDispatchResult::IntentApiUnavailable;
    }
    struct LocalFrame {
        JNIEnv* env;
        ~LocalFrame() { env->PopLocalFrame(nullptr); }
    } frame{env};

    jobject app = currentApplication(env);
    if (!app) return KeyMapperDispatchResult::ContextUnavailable;

    jclass intentClass = env->FindClass("android/content/Intent");
    if (!intentClass || clearException(env)) return KeyMapperDispatchResult::IntentApiUnavailable;
    const jmethodID intentCtor = env->GetMethodID(intentClass, "<init>", "()V");
    const jmethodID setAction = env->GetMethodID(
        intentClass, "setAction", "(Ljava/lang/String;)Landroid/content/Intent;");
    const jmethodID setComponent = env->GetMethodID(
        intentClass, "setComponent", "(Landroid/content/ComponentName;)Landroid/content/Intent;");
    const jmethodID putExtra = env->GetMethodID(
        intentClass, "putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;");
    if (clearException(env) || !intentCtor || !setAction || !setComponent || !putExtra)
        return KeyMapperDispatchResult::IntentApiUnavailable;

    jclass componentClass = env->FindClass("android/content/ComponentName");
    if (!componentClass || clearException(env)) return KeyMapperDispatchResult::IntentApiUnavailable;
    const jmethodID componentCtor = env->GetMethodID(
        componentClass, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (!componentCtor || clearException(env)) return KeyMapperDispatchResult::IntentApiUnavailable;

    jobject intent = env->NewObject(intentClass, intentCtor);
    if (!intent || clearException(env)) return KeyMapperDispatchResult::IntentApiUnavailable;

    jstring action = env->NewStringUTF(kAction);
    jstring packageName = env->NewStringUTF(kPackage);
    jstring receiver = env->NewStringUTF(kReceiver);
    jstring extra = env->NewStringUTF(kUidExtra);
    const std::string uidString(uid);
    jstring uidValue = env->NewStringUTF(uidString.c_str());
    if (!action || !packageName || !receiver || !extra || !uidValue || clearException(env))
        return KeyMapperDispatchResult::IntentApiUnavailable;

    jobject component = env->NewObject(componentClass, componentCtor, packageName, receiver);
    if (!component || clearException(env)) return KeyMapperDispatchResult::IntentApiUnavailable;

    env->CallObjectMethod(intent, setAction, action);
    if (clearException(env)) return KeyMapperDispatchResult::IntentApiUnavailable;
    env->CallObjectMethod(intent, setComponent, component);
    if (clearException(env)) return KeyMapperDispatchResult::IntentApiUnavailable;
    env->CallObjectMethod(intent, putExtra, extra, uidValue);
    if (clearException(env)) return KeyMapperDispatchResult::IntentApiUnavailable;

    jclass contextClass = env->FindClass("android/content/Context");
    if (!contextClass || clearException(env)) return KeyMapperDispatchResult::IntentApiUnavailable;
    const jmethodID sendBroadcast = env->GetMethodID(
        contextClass, "sendBroadcast", "(Landroid/content/Intent;)V");
    if (!sendBroadcast || clearException(env)) return KeyMapperDispatchResult::IntentApiUnavailable;

    env->CallVoidMethod(app, sendBroadcast, intent);
    if (clearException(env)) return KeyMapperDispatchResult::BroadcastFailed;
    return KeyMapperDispatchResult::Sent;
}

const char* keyMapperDispatchResultName(KeyMapperDispatchResult result) noexcept {
    switch (result) {
        case KeyMapperDispatchResult::Sent: return "broadcast_sent";
        case KeyMapperDispatchResult::InvalidUid: return "invalid_keymap_uid";
        case KeyMapperDispatchResult::VmUnavailable: return "android_vm_unavailable";
        case KeyMapperDispatchResult::ThreadAttachFailed: return "android_thread_attach_failed";
        case KeyMapperDispatchResult::ContextUnavailable: return "android_context_unavailable";
        case KeyMapperDispatchResult::IntentApiUnavailable: return "keymapper_intent_api_unavailable";
        case KeyMapperDispatchResult::BroadcastFailed: return "keymapper_broadcast_failed";
    }
    return "unknown";
}
} // namespace bedrocktools::core