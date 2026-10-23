#include "autohit.hpp"

#include "aimassist.hpp"
#include "AutoHitGuards.hpp"
#include "AutoHitWeaponProfile.hpp"
#include "../../core/Diagnostics.hpp"
#include "../../core/JavaVm.hpp"
#include "core/InventoryAccess.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/events/GameModeActionEvent.hpp>
#include <bedrocktools/events/ScreenStateEvent.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <charconv>
#include <optional>
#include <unordered_set>
#include <dlfcn.h>
#include <jni.h>
#include <cmath>
#include <cctype>
#include <string>
#include <utility>

namespace {
constexpr const char* kKeyMapperPackage = "io.github.sds100.keymapper";
constexpr const char* kKeyMapperReceiver = "io.github.sds100.keymapper.api.TriggerKeyMapsBroadcastReceiver";
constexpr const char* kKeyMapperAction = "io.github.sds100.keymapper.ACTION_TRIGGER_KEYMAP_BY_UID";
constexpr const char* kKeyMapperUidExtra = "io.github.sds100.keymapper.EXTRA_KEYMAP_UID";
constexpr const char* kKeyMapperKeymapUid = "fb36fd6d-8811-4a87-8ca4-bf970a6fcb83";

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
class AutoHitJniEnvironment {
public:
    AutoHitJniEnvironment() {
        // Match the previously working Key Mapper route: prefer ART's process VM
        // lookup. The Preloader-captured VM remains a fallback for linker/runtime
        // configurations where JNI_GetCreatedJavaVMs cannot be resolved.
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
    ~AutoHitJniEnvironment() { if (mAttached && mVm) mVm->DetachCurrentThread(); }
    JNIEnv* get() const noexcept { return mEnv; }
    VmLookupStatus lookupStatus() const noexcept { return status; }
private:
    JavaVM* mVm = nullptr; JNIEnv* mEnv = nullptr; bool mAttached = false;
    VmLookupStatus status = VmLookupStatus::SymbolUnavailable;
};


bool clearAutoHitJniException(JNIEnv* env) {
    if (!env || !env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

enum class KeyZeroDispatchResult { Sent, VmSymbolUnavailable, VmQueryFailed, VmNotFound, ThreadAttachFailed, ContextUnavailable, IntentUnavailable, BroadcastFailed };
KeyZeroDispatchResult dispatchKeyMapperKeyZero() {
    AutoHitJniEnvironment jni;
    JNIEnv* env = jni.get();
    if (!env) {
        switch (jni.lookupStatus()) {
            case VmLookupStatus::SymbolUnavailable: return KeyZeroDispatchResult::VmSymbolUnavailable;
            case VmLookupStatus::QueryFailed: return KeyZeroDispatchResult::VmQueryFailed;
            case VmLookupStatus::NoVmFound: return KeyZeroDispatchResult::VmNotFound;
            case VmLookupStatus::AttachFailed: return KeyZeroDispatchResult::ThreadAttachFailed;
            case VmLookupStatus::Ready: break;
        }
        return KeyZeroDispatchResult::VmSymbolUnavailable;
    }
    if (env->PushLocalFrame(40) != JNI_OK) { clearAutoHitJniException(env); return KeyZeroDispatchResult::IntentUnavailable; }
    struct LocalFrameGuard { JNIEnv* env; ~LocalFrameGuard() { env->PopLocalFrame(nullptr); } } frame{env};
    jobject app = nullptr;
    jclass threadClass = env->FindClass("android/app/ActivityThread");
    if (threadClass && !clearAutoHitJniException(env)) {
        jmethodID m = env->GetStaticMethodID(threadClass, "currentApplication", "()Landroid/app/Application;");
        if (m && !clearAutoHitJniException(env)) { app = env->CallStaticObjectMethod(threadClass, m); if (clearAutoHitJniException(env)) app = nullptr; }
        else clearAutoHitJniException(env);
    } else clearAutoHitJniException(env);
    if (!app) {
        jclass globals = env->FindClass("android/app/AppGlobals");
        if (globals && !clearAutoHitJniException(env)) {
            jmethodID m = env->GetStaticMethodID(globals, "getInitialApplication", "()Landroid/app/Application;");
            if (m && !clearAutoHitJniException(env)) { app = env->CallStaticObjectMethod(globals, m); if (clearAutoHitJniException(env)) app = nullptr; }
            else clearAutoHitJniException(env);
        } else clearAutoHitJniException(env);
    }
    if (!app) return KeyZeroDispatchResult::ContextUnavailable;
    jclass ic = env->FindClass("android/content/Intent");
    if (!ic || clearAutoHitJniException(env)) return KeyZeroDispatchResult::IntentUnavailable;
    jmethodID ctor=env->GetMethodID(ic,"<init>","()V");
    jmethodID setAction=env->GetMethodID(ic,"setAction","(Ljava/lang/String;)Landroid/content/Intent;");
    jmethodID setComponent=env->GetMethodID(ic,"setComponent","(Landroid/content/ComponentName;)Landroid/content/Intent;");
    jmethodID putExtra=env->GetMethodID(ic,"putExtra","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;");
    if (clearAutoHitJniException(env)||!ctor||!setAction||!setComponent||!putExtra) return KeyZeroDispatchResult::IntentUnavailable;
    jclass cc=env->FindClass("android/content/ComponentName");
    if (!cc||clearAutoHitJniException(env)) return KeyZeroDispatchResult::IntentUnavailable;
    jmethodID cctor=env->GetMethodID(cc,"<init>","(Ljava/lang/String;Ljava/lang/String;)V");
    if (!cctor||clearAutoHitJniException(env)) return KeyZeroDispatchResult::IntentUnavailable;
    jobject intent=env->NewObject(ic,ctor);
    if (!intent||clearAutoHitJniException(env)) return KeyZeroDispatchResult::IntentUnavailable;
    jstring action=env->NewStringUTF(kKeyMapperAction), pkg=env->NewStringUTF(kKeyMapperPackage);
    jstring receiver=env->NewStringUTF(kKeyMapperReceiver), extra=env->NewStringUTF(kKeyMapperUidExtra);
    jstring uid=env->NewStringUTF(kKeyMapperKeymapUid);
    if (!action||!pkg||!receiver||!extra||!uid||clearAutoHitJniException(env)) return KeyZeroDispatchResult::IntentUnavailable;
    jobject component=env->NewObject(cc,cctor,pkg,receiver);
    if (!component||clearAutoHitJniException(env)) return KeyZeroDispatchResult::IntentUnavailable;
    env->CallObjectMethod(intent,setAction,action); if(clearAutoHitJniException(env)) return KeyZeroDispatchResult::IntentUnavailable;
    env->CallObjectMethod(intent,setComponent,component); if(clearAutoHitJniException(env)) return KeyZeroDispatchResult::IntentUnavailable;
    env->CallObjectMethod(intent,putExtra,extra,uid); if(clearAutoHitJniException(env)) return KeyZeroDispatchResult::IntentUnavailable;
    jclass context=env->FindClass("android/content/Context");
    if (!context||clearAutoHitJniException(env)) return KeyZeroDispatchResult::IntentUnavailable;
    jmethodID send=env->GetMethodID(context,"sendBroadcast","(Landroid/content/Intent;)V");
    if (!send||clearAutoHitJniException(env)) return KeyZeroDispatchResult::IntentUnavailable;
    env->CallVoidMethod(app,send,intent);
    if(clearAutoHitJniException(env)) return KeyZeroDispatchResult::BroadcastFailed;
    return KeyZeroDispatchResult::Sent;
}
const char* keyZeroDispatchResultName(KeyZeroDispatchResult result) {
    switch(result) {
        case KeyZeroDispatchResult::Sent: return "keymapper_key0_broadcast_sent";
        case KeyZeroDispatchResult::VmSymbolUnavailable: return "android_jni_vm_symbol_unavailable";
        case KeyZeroDispatchResult::VmQueryFailed: return "android_jni_vm_query_failed";
        case KeyZeroDispatchResult::VmNotFound: return "android_jni_vm_not_found";
        case KeyZeroDispatchResult::ThreadAttachFailed: return "android_jni_thread_attach_failed";
        case KeyZeroDispatchResult::ContextUnavailable: return "android_context_unavailable";
        case KeyZeroDispatchResult::IntentUnavailable: return "keymapper_intent_api_unavailable";
        case KeyZeroDispatchResult::BroadcastFailed: return "keymapper_broadcast_failed";
    }
    return "keymapper_broadcast_unknown";
}

thread_local bool g_autoHitSyntheticAttack = false;

using bedrocktools::sdk::AABB;
using bedrocktools::sdk::Player;
using bedrocktools::sdk::Vec3;

bool finiteVec(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool validBounds(const AABB& bounds) {
    if (!finiteVec(bounds.min) || !finiteVec(bounds.max)) return false;
    const float width = bounds.max.x - bounds.min.x;
    const float height = bounds.max.y - bounds.min.y;
    const float depth = bounds.max.z - bounds.min.z;
    return width > 0.0f && width <= 5.0f && height >= 0.25f && height <= 8.0f &&
           depth > 0.0f && depth <= 5.0f;
}

float distanceSquared(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

Vec3 eyePosition(Player* player) {
    const Vec3 position = player->position();
    const AABB bounds = player->bounds();
    if (validBounds(bounds)) {
        return {(bounds.min.x + bounds.max.x) * 0.5f,
                bounds.min.y + (bounds.max.y - bounds.min.y) * 0.85f,
                (bounds.min.z + bounds.max.z) * 0.5f};
    }
    Vec3 eye = position;
    eye.y += 1.5f;
    return eye;
}

using WeaponCustomProfile = bedaim::autohit::weapon::CustomProfile;
using WeaponCatalogEntry = bedaim::autohit::weapon::CatalogEntry;

WeaponCustomProfile makeLegacyProfile(const WeaponCatalogEntry& entry, float range,
                                      bool swordEnabled, float swordCps,
                                      bool axeEnabled, float axeCps,
                                      bool maceEnabled, float maceCps,
                                      bool tridentEnabled, float tridentCps) {
    using namespace bedaim::autohit::weapon;
    const Class category = classify(entry.identifier);
    const Profile old = resolve(category, swordEnabled, swordCps, axeEnabled, axeCps,
                                maceEnabled, maceCps, tridentEnabled, tridentCps);
    WeaponCustomProfile profile;
    profile.identifier = std::string(entry.identifier);
    profile.displayName = std::string(entry.displayName);
    profile.enabled = old.enabled;
    profile.range = normalizeRange(range);
    profile.cps = old.cps > 0.0f ? normalizeNormalCps(old.cps) : 4.0f;
    if (category == Class::Spear) {
        profile.enabled = false;
        profile.cps = 3.0f;
    } else if (category == Class::Bow || category == Class::Crossbow) {
        profile.enabled = false;
        profile.cps = 4.0f;
    }
    profile.initialBurstHits = 0;
    profile.initialBurstCps = 13.0f;
    return normalizeCustomProfile(std::move(profile));
}

bool parseIndexSuffix(std::string_view key, std::string_view prefix, std::size_t& index) {
    if (!key.starts_with(prefix)) return false;
    const auto suffix = key.substr(prefix.size());
    if (suffix.empty()) return false;
    std::size_t parsed = 0;
    const auto result = std::from_chars(suffix.data(), suffix.data() + suffix.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != suffix.data() + suffix.size()) return false;
    index = parsed;
    return true;
}

bool parseBoolValue(std::string_view value, bool& out) {
    if (value == "true" || value == "1" || value == "on" || value == "yes") {
        out = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "off" || value == "no") {
        out = false;
        return true;
    }
    return false;
}

}

AutoHitModule::AutoHitModule()
    : Module("Auto Hit",
             "Experimental held-attack helper; requires BedrockAim to provide a fresh target.") {
    keybind = 0;
}

void AutoHitModule::onInit() {
    bedrocktools::core::InventoryAccess::get().initialize();
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { processPlayerTick(event.player); });
    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>(
        [this](auto& event) {
            if (event.screen == bedrocktools::events::ScreenKind::Container) {
                const bool opened = event.phase == bedrocktools::events::ScreenPhase::Opened;
                m_containerOpen.store(opened, std::memory_order_release);
                if (opened) {
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    m_attackHeld = false;
                    m_heldButton = -1;
                    m_touchRepeatUntil = {};
                }
            } else if (event.screen == bedrocktools::events::ScreenKind::Chat) {
                const bool opened = event.phase == bedrocktools::events::ScreenPhase::Opened;
                m_chatOpen.store(opened, std::memory_order_release);
                if (opened) {
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    m_attackHeld = false;
                    m_heldButton = -1;
                    m_touchRepeatUntil = {};
                }
            }
        });
    // The launcher mouse callback may not receive Bedrock's native touchscreen
    // attack button. Observe ordinary engine attack actions as a mobile-compatible
    // activation signal, then permit a bounded repeat window. Synthetic attacks
    // are explicitly ignored so AutoHit cannot re-arm itself.
    bedrocktools::events::bus().subscribe<bedrocktools::events::GameModeActionEvent>(
        [this](auto& event) {
            const bool nativeAttack = event.action == bedrocktools::events::GameModeAction::Attack;
            const bool itemAsAttack = event.action == bedrocktools::events::GameModeAction::UseItemAsAttack;
            if (!nativeAttack && !itemAsAttack) return;

            const char* actionName = nativeAttack ? "Attack" : "UseItemAsAttack";
            if (!enabled) return;
            if (g_autoHitSyntheticAttack) {
                bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Trace,
                    "AutoHit", "touch_activation", "synthetic_action_ignored", actionName);
                return;
            }
            if (m_containerOpen.load(std::memory_order_acquire) ||
                m_chatOpen.load(std::memory_order_acquire)) {
                bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
                    "AutoHit", "touch_activation", "blocked_by_screen", actionName);
                return;
            }

            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (!m_touchRepeatEnabled) {
                bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
                    "AutoHit", "touch_activation",
                    nativeAttack ? "attack_seen_repeat_disabled" : "item_as_attack_seen_repeat_disabled",
                    "native action was observed, but touchscreen repeat is disabled");
                return;
            }
            const auto window = std::chrono::milliseconds(std::clamp(m_touchRepeatWindowMs, 100, 1000));
            m_touchRepeatUntil = std::chrono::steady_clock::now() + window;
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
                "AutoHit", "touch_input",
                nativeAttack ? "attack_repeat_window_armed" : "item_as_attack_repeat_window_armed",
                std::string("source=") + actionName + "; window_ms=" + std::to_string(window.count()));
        });
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "initialized",
        "target-gated continuous attacks through the configured Key Mapper keymap");
}

void AutoHitModule::onEnable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackHeld = false;
    m_heldButton = -1;
    m_touchRepeatUntil = {};
    m_nextAttack = {};
    m_burstWeaponIdentifier.clear();
    m_burstTarget = nullptr;
    m_burstRemaining = 0;
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "enabled");
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "activation_mode", m_requireAttackInput ? "input_required" : "automatic_when_enabled");
}

void AutoHitModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackHeld = false;
    m_heldButton = -1;
    m_nextAttack = {};
    m_burstWeaponIdentifier.clear();
    m_burstTarget = nullptr;
    m_burstRemaining = 0;
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "disabled");
}

bool AutoHitModule::onMouseEvent(int button, bool isDown) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    const bool screenBlocked = m_containerOpen.load(std::memory_order_acquire) ||
        m_chatOpen.load(std::memory_order_acquire);
    if (screenBlocked) {
        m_attackHeld = false;
        m_heldButton = -1;
        return false;
    }
    if (enabled) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Trace,
            "AutoHit", "raw_mouse_input",
            std::string("button_") + std::to_string(button) + (isDown ? "_down" : "_up"),
            "raw button index observed by the launcher input bridge");
    }
    if (!enabled) {
        if (!isDown && button == m_heldButton) {
            m_attackHeld = false;
            m_heldButton = -1;
        }
        return false; // Never consume or cancel the user's normal mouse input.
    }

    if (isDown && button == m_attackButton) {
        m_attackHeld = true;
        m_heldButton = button;
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "AutoHit", "input", "attack_button_held");
    } else if (!isDown && button == m_heldButton) {
        m_attackHeld = false;
        m_heldButton = -1;
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "AutoHit", "input", "attack_button_released");
    }
    return false;
}

void AutoHitModule::processPlayerTick(Player* player) {
    using namespace bedrocktools;
    if (!enabled) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "dispatch_gate", "disabled");
        return;
    }
    if (!player) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "dispatch_gate", "missing_player");
        return;
    }
    if (m_containerOpen.load(std::memory_order_acquire) ||
        m_chatOpen.load(std::memory_order_acquire)) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "dispatch_gate", "screen_open");
        return;
    }

    // The active AutoHit input route is the existing Key Mapper keymap. Keep this
    // dispatcher unchanged; this section only resolves the selected item's profile.
    auto& inventory = core::InventoryAccess::get();
    const std::string heldItem = inventory.selectedHotbarIdentifier(player);
    const std::string normalizedItem = bedaim::autohit::weapon::normalizeIdentifier(heldItem);
    std::optional<WeaponCustomProfile> customProfile;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        const auto found = std::find_if(m_customWeaponProfiles.begin(), m_customWeaponProfiles.end(),
            [&](const auto& item) {
                return item.identifier == normalizedItem;
            });
        if (found != m_customWeaponProfiles.end()) customProfile = *found;

        // A switch to a different held item breaks the old burst cycle immediately.
        // Preserve the shared cooldown so transient empty item reads cannot bypass
        // the steady Key Mapper request rate.
        if (m_burstWeaponIdentifier != normalizedItem) {
            m_burstWeaponIdentifier = normalizedItem;
            m_burstTarget = nullptr;
            m_burstRemaining = 0;
        }
    }
    if (!customProfile) {
        diagnostics::probe(diagnostics::Level::Info, "AutoHit", "weapon_profile", "no_item_profile",
            std::string("item=") + (normalizedItem.empty() ? "empty_or_unreadable" : normalizedItem) +
                "; add this item to Weapon Profiles to enable AutoHit for it");
        return;
    }

    *customProfile = bedaim::autohit::weapon::normalizeCustomProfile(std::move(*customProfile));
    if (!customProfile->enabled) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "weapon_profile", "disabled",
            std::string("item=") + customProfile->identifier);
        return;
    }
    float configuredRange = customProfile->range;
    float configuredCps = customProfile->cps;
    diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "weapon_profile", "allowed",
        std::string("item=") + customProfile->identifier + "; cps=" + std::to_string(configuredCps) +
            "; range=" + std::to_string(configuredRange));

    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (!aimBase || !aimBase->enabled) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "dependency", "bedrockaim_disabled",
            "enable BedrockAim to provide target tracking");
        return;
    }

    auto* aim = static_cast<AimAssistModule*>(aimBase);
    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    if (!aim->currentTargetSnapshot(target, targetBounds, aimRange) || !target) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "target", "missing_or_stale");
        return;
    }
    if (!validBounds(targetBounds)) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "target", "invalid_bounds");
        return;
    }
    if (!aim->currentTargetInCrosshair(player, target)) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "crosshair", "target_not_centered");
        return;
    }
    diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "crosshair", "confirmed");

    const Vec3 eye = eyePosition(player);
    if (!finiteVec(eye)) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "hit_point", "invalid_player_eye");
        return;
    }
    const Vec3 hitPoint{
        std::clamp(eye.x, targetBounds.min.x, targetBounds.max.x),
        std::clamp(eye.y, targetBounds.min.y, targetBounds.max.y),
        std::clamp(eye.z, targetBounds.min.z, targetBounds.max.z)
    };
    const float distanceSq = distanceSquared(eye, hitPoint);
    if (!std::isfinite(aimRange) || aimRange <= 0.0f) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "range", "aim_range_invalid");
        return;
    }
    // The per-item profile provides its own maximum; current Aim Assist tracking
    // range remains an independent upper bound.
    const float maxRange = std::clamp(std::min(configuredRange, aimRange), 1.0f, 6.0f);
    if (!finiteVec(hitPoint) || !std::isfinite(distanceSq) || distanceSq > maxRange * maxRange) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "range", "out_of_range");
        return;
    }
    diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "range", "in_range");

    const auto now = std::chrono::steady_clock::now();
    bool usingInitialBurst = false;
    int intervalMs = bedaim::autohit::weapon::attemptIntervalMs(configuredCps);
    int burstRemaining = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        // Re-arm only when the selected weapon or target changes, never on each tick.
        // Do not clear m_nextAttack here: target/weapon churn must not bypass the
        // rate limiter used to pace Key Mapper's asynchronous keymap execution.
        if (m_burstWeaponIdentifier != normalizedItem || m_burstTarget != target) {
            m_burstWeaponIdentifier = normalizedItem;
            m_burstTarget = target;
            m_burstRemaining = customProfile->initialBurstHits;
        }
        if (m_burstRemaining > 0) {
            configuredCps = customProfile->initialBurstCps;
            usingInitialBurst = true;
        }
        intervalMs = bedaim::autohit::weapon::attemptIntervalMs(configuredCps);
        burstRemaining = m_burstRemaining;
        if (m_nextAttack.time_since_epoch().count() != 0 && now < m_nextAttack) {
            diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "cooldown", "waiting");
            return;
        }
    }

    // Final Key Mapper gate ensures target, geometry, range and pacing are valid.
    const bedaim::autohit::Inputs dispatchFacts{
        enabled,
        player != nullptr,
        target != nullptr,
        validBounds(targetBounds),
        true, // currentTargetSnapshot rejects stale target data
        finiteVec(hitPoint),
        true, // shared cooldown was checked immediately before dispatch
        std::sqrt(distanceSq),
        maxRange
    };
    const auto dispatchDecision = bedaim::autohit::evaluate(dispatchFacts);
    if (dispatchDecision != bedaim::autohit::Decision::Ready) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "final_guard",
            "blocked", "Key Mapper dispatch prerequisites failed validation");
        return;
    }

    const auto dispatchResult = dispatchKeyMapperKeyZero();
    const bool dispatched = dispatchResult == KeyZeroDispatchResult::Sent;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        // Normal CPS remains capped at 12. Burst CPS may reach 20 CPS and is
        // rate-limited even when Android rejects the broadcast.
        if (dispatched && usingInitialBurst && m_burstWeaponIdentifier == normalizedItem &&
            m_burstTarget == target && m_burstRemaining > 0) {
            --m_burstRemaining;
        }
        const bool burstJustEnded = dispatched && usingInitialBurst && m_burstRemaining == 0;
        const int nextIntervalMs = burstJustEnded
            ? bedaim::autohit::weapon::attemptIntervalMs(customProfile->cps)
            : intervalMs;
        m_nextAttack = now + std::chrono::milliseconds(std::clamp(nextIntervalMs, 50, 1000));
        burstRemaining = m_burstRemaining;
    }
    diagnostics::probe(dispatched ? diagnostics::Level::Info : diagnostics::Level::Warning,
        "AutoHit", "key0_dispatch", keyZeroDispatchResultName(dispatchResult),
        dispatched
            ? "Android accepted the explicit Key Mapper broadcast for the configured keymap UUID; this does not prove Key Mapper executed the keymap or Minecraft received key 0."
            : "Key Mapper trigger dispatch failed before key delivery; inspect Android VM/context/Intent diagnostics");
    if (usingInitialBurst) {
        diagnostics::probe(dispatched ? diagnostics::Level::Info : diagnostics::Level::Debug,
            "AutoHit", "initial_burst", dispatched ? "request_accepted" : "request_failed",
            std::string("item=") + normalizedItem + "; remaining_requests=" +
                std::to_string(burstRemaining) +
                "; accepted broadcast requests are counted, not confirmed in-game damage hits");
    }
}

void AutoHitModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackButton = std::clamp(json.value("m_attackButton", m_attackButton), 0, 16);
    // Keep legacy values for migration and for the default values used when an
    // item profile is first introduced. They are no longer exposed as global
    // per-class toggles in the UI.
    m_requireAttackInput = json.value("m_requireAttackInput", m_requireAttackInput);
    m_attackRange = std::clamp(json.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
    m_touchRepeatEnabled = json.value("m_touchRepeatEnabled", m_touchRepeatEnabled);
    m_touchRepeatWindowMs = std::clamp(json.value("m_touchRepeatWindowMs", m_touchRepeatWindowMs), 100, 1000);
    m_swordEnabled = json.value("m_swordEnabled", m_swordEnabled);
    m_swordCps = std::clamp(json.value("m_swordCps", 5.0f), 1.0f, 12.0f);
    m_axeEnabled = json.value("m_axeEnabled", m_axeEnabled);
    m_axeCps = std::clamp(json.value("m_axeCps", m_axeCps), 1.0f, 12.0f);
    m_maceEnabled = json.value("m_maceEnabled", m_maceEnabled);
    m_maceCps = std::clamp(json.value("m_maceCps", m_maceCps), 1.0f, 12.0f);
    m_tridentEnabled = json.value("m_tridentEnabled", m_tridentEnabled);
    m_tridentCps = std::clamp(json.value("m_tridentCps", m_tridentCps), 1.0f, 12.0f);

    m_customWeaponProfiles.clear();
    const auto profilesIt = json.find("m_customWeaponProfiles");
    if (profilesIt != json.end() && profilesIt->is_array()) {
        std::unordered_set<std::string> seen;
        for (const auto& item : *profilesIt) {
            if (!item.is_object() || m_customWeaponProfiles.size() >= 120) continue;
            WeaponCustomProfile profile;
            profile.identifier = item.value("identifier", std::string{});
            profile.displayName = item.value("displayName", std::string{});
            profile.enabled = item.value("enabled", false);
            profile.range = item.value("range", m_attackRange);
            profile.cps = item.value("cps", 5.0f);
            profile.initialBurstHits = item.value("initialBurstHits", 0);
            profile.initialBurstCps = item.value("initialBurstCps", 13.0f);
            profile = bedaim::autohit::weapon::normalizeCustomProfile(std::move(profile));
            if (profile.identifier.empty() || !seen.insert(profile.identifier).second) continue;
            m_customWeaponProfiles.push_back(std::move(profile));
        }
    } else {
        // One-time migration from the former sword/axe/mace/trident settings.
        // Expand each class default into individual item IDs so every weapon can
        // subsequently have an independent range, CPS and initial burst.
        for (const auto& entry : bedaim::autohit::weapon::kWeaponCatalog) {
            m_customWeaponProfiles.push_back(makeLegacyProfile(
                entry, m_attackRange,
                m_swordEnabled, m_swordCps,
                m_axeEnabled, m_axeCps,
                m_maceEnabled, m_maceCps,
                m_tridentEnabled, m_tridentCps));
        }
    }

    m_addWeaponChoice = bedaim::autohit::weapon::normalizeIdentifier(
        json.value("m_addWeaponChoice", std::string("minecraft:mace")));
    const bool knownChoice = std::any_of(
        bedaim::autohit::weapon::kWeaponCatalog.begin(),
        bedaim::autohit::weapon::kWeaponCatalog.end(),
        [&](const auto& entry) { return entry.identifier == m_addWeaponChoice; });
    if (!knownChoice) m_addWeaponChoice = "minecraft:mace";

    m_customWeaponIdentifier = json.value("m_customWeaponIdentifier", std::string{});
    if (m_customWeaponIdentifier.size() > 128) m_customWeaponIdentifier.resize(128);
    m_selectedProfileIdentifier = bedaim::autohit::weapon::normalizeIdentifier(
        json.value("m_selectedProfileIdentifier", std::string("minecraft:mace")));
    const auto selectedIt = std::find_if(m_customWeaponProfiles.begin(), m_customWeaponProfiles.end(),
        [&](const auto& profile) { return profile.identifier == m_selectedProfileIdentifier; });
    if (selectedIt == m_customWeaponProfiles.end()) {
        m_selectedProfileIdentifier = m_customWeaponProfiles.empty()
            ? std::string{} : m_customWeaponProfiles.front().identifier;
    }
    m_burstWeaponIdentifier.clear();
    m_burstTarget = nullptr;
    m_burstRemaining = 0;
    m_nextAttack = {};
}

void AutoHitModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    json["m_attackButton"] = m_attackButton;
    json["m_requireAttackInput"] = m_requireAttackInput;
    json["m_attackRange"] = m_attackRange;
    json["m_touchRepeatEnabled"] = m_touchRepeatEnabled;
    json["m_touchRepeatWindowMs"] = m_touchRepeatWindowMs;
    json["m_swordEnabled"] = m_swordEnabled;
    json["m_swordCps"] = m_swordCps;
    json["m_axeEnabled"] = m_axeEnabled;
    json["m_axeCps"] = m_axeCps;
    json["m_maceEnabled"] = m_maceEnabled;
    json["m_maceCps"] = m_maceCps;
    json["m_tridentEnabled"] = m_tridentEnabled;
    json["m_tridentCps"] = m_tridentCps;

    auto profiles = nlohmann::json::array();
    for (const auto& raw : m_customWeaponProfiles) {
        const auto profile = bedaim::autohit::weapon::normalizeCustomProfile(raw);
        profiles.push_back({
            {"identifier", profile.identifier},
            {"displayName", profile.displayName},
            {"enabled", profile.enabled},
            {"range", profile.range},
            {"cps", profile.cps},
            {"initialBurstHits", profile.initialBurstHits},
            {"initialBurstCps", profile.initialBurstCps}
        });
    }
    json["m_customWeaponProfiles"] = std::move(profiles);
    json["m_addWeaponChoice"] = m_addWeaponChoice;
    json["m_customWeaponIdentifier"] = m_customWeaponIdentifier;
    json["m_selectedProfileIdentifier"] = m_selectedProfileIdentifier;
}

void AutoHitModule::onMenuRegistered() {
    using namespace pl::modmenu;

    std::vector<WeaponCustomProfile> profiles;
    std::string addChoice;
    std::string customIdentifier;
    std::string selectedIdentifier;
    float defaultRange = 3.2f;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        profiles = m_customWeaponProfiles;
        addChoice = m_addWeaponChoice;
        customIdentifier = m_customWeaponIdentifier;
        selectedIdentifier = m_selectedProfileIdentifier;
        defaultRange = m_attackRange;
    }

    ConfigSchemaBuilder schema;
    schema.defaultCategory("weapon_profiles")
        .category("weapon_profiles", "Weapon Profiles", "Create and manage independent settings for each held item")
        .category("profile_settings", "Profile Settings", "Range, normal CPS and initial burst for the selected weapon")
        .category("combat", "Combat", "General AutoHit settings");

    ConfigNodeV2 weaponChoice;
    weaponChoice.id = "m_addWeaponChoice";
    weaponChoice.key = "m_addWeaponChoice";
    weaponChoice.title = "Choose weapon";
    weaponChoice.description = "Choose a vanilla weapon, or enter an add-on item identifier below.";
    weaponChoice.category = "weapon_profiles";
    weaponChoice.type = ConfigControlTypeV2::Choice;
    weaponChoice.choiceStyle = ConfigChoiceStyleV2::Dropdown;
    weaponChoice.defaultValue = "minecraft:mace";
    weaponChoice.currentValue = addChoice;
    weaponChoice.hasCurrentValue = true;
    weaponChoice.searchable = true;
    for (const auto& entry : bedaim::autohit::weapon::kWeaponCatalog) {
        ConfigOptionV2 option;
        option.value = std::string(entry.identifier);
        option.label = std::string(entry.displayName);
        weaponChoice.options.push_back(std::move(option));
    }
    schema.node(std::move(weaponChoice));

    ConfigNodeV2 customId;
    customId.id = "m_customWeaponIdentifier";
    customId.key = "m_customWeaponIdentifier";
    customId.title = "Custom item identifier (optional)";
    customId.description = "For add-ons, enter the exact item ID, for example myaddon:custom_sword. This field overrides the dropdown when non-empty.";
    customId.category = "weapon_profiles";
    customId.type = ConfigControlTypeV2::Text;
    customId.defaultValue = "";
    customId.currentValue = customIdentifier;
    customId.hasCurrentValue = true;
    customId.placeholder = "myaddon:custom_sword";
    customId.maxLength = 128;
    schema.node(std::move(customId));

    ConfigNodeV2 addButton;
    addButton.id = "m_addWeaponButton";
    addButton.key = "m_addWeaponButton";
    addButton.title = "Add Weapon";
    addButton.description = "Add the selected item to the profile bench. Existing IDs are selected rather than duplicated.";
    addButton.category = "weapon_profiles";
    addButton.type = ConfigControlTypeV2::Button;
    addButton.actionValue = "add";
    schema.node(std::move(addButton));

    ConfigNodeV2 profileListSection;
    profileListSection.id = "m_weapon_profile_list";
    profileListSection.title = "Your weapons";
    profileListSection.description = "Use Settings to edit a profile, Switch to enable it, or Delete to remove it.";
    profileListSection.category = "weapon_profiles";
    profileListSection.type = ConfigControlTypeV2::Section;
    profileListSection.collapsible = true;
    schema.node(std::move(profileListSection));

    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const auto& profile = profiles[i];
        const std::string index = std::to_string(i);
        const bool isSelected = profile.identifier == selectedIdentifier;
        ConfigNodeV2 section;
        section.id = "m_profile_section_" + index;
        section.title = profile.displayName + (isSelected ? " — Settings (selected)" : " — Settings");
        section.description = profile.identifier;
        section.category = "weapon_profiles";
        section.type = ConfigControlTypeV2::Section;
        schema.node(std::move(section));

        ConfigNodeV2 enabledNode;
        enabledNode.id = "m_profile_enabled_" + index;
        enabledNode.key = enabledNode.id;
        enabledNode.title = "Switch: " + profile.displayName;
        enabledNode.description = "When off, AutoHit will never dispatch for this exact item ID.";
        enabledNode.category = "weapon_profiles";
        enabledNode.type = ConfigControlTypeV2::Toggle;
        enabledNode.defaultValue = profile.enabled ? "true" : "false";
        enabledNode.currentValue = profile.enabled ? "true" : "false";
        enabledNode.hasCurrentValue = true;
        schema.node(std::move(enabledNode));

        ConfigNodeV2 settingsButton;
        settingsButton.id = "m_profile_settings_" + index;
        settingsButton.key = settingsButton.id;
        settingsButton.title = isSelected ? "Settings ✓" : "Settings";
        settingsButton.description = "Open this item's range, CPS and Initial Burst settings.";
        settingsButton.category = "weapon_profiles";
        settingsButton.type = ConfigControlTypeV2::Button;
        settingsButton.actionValue = "settings";
        schema.node(std::move(settingsButton));

        ConfigNodeV2 deleteButton;
        deleteButton.id = "m_profile_delete_" + index;
        deleteButton.key = deleteButton.id;
        deleteButton.title = "Delete: " + profile.displayName;
        deleteButton.description = "Remove this exact item profile. The item will be blocked until a profile is added again.";
        deleteButton.category = "weapon_profiles";
        deleteButton.type = ConfigControlTypeV2::Button;
        deleteButton.actionValue = "delete";
        schema.node(std::move(deleteButton));
    }

    ConfigNodeV2 defaultRangeNode;
    defaultRangeNode.id = "m_attackRange";
    defaultRangeNode.key = "m_attackRange";
    defaultRangeNode.title = "Default range for newly added profiles";
    defaultRangeNode.description = "Existing profiles keep their own range.";
    defaultRangeNode.category = "combat";
    defaultRangeNode.type = ConfigControlTypeV2::SliderFloat;
    defaultRangeNode.defaultValue = "3.2";
    defaultRangeNode.currentValue = std::to_string(defaultRange);
    defaultRangeNode.hasCurrentValue = true;
    defaultRangeNode.minValue = "2";
    defaultRangeNode.maxValue = "6";
    defaultRangeNode.step = "0.1";
    defaultRangeNode.unit = " blocks";
    schema.node(std::move(defaultRangeNode));

    ConfigNodeV2 keybindNode;
    keybindNode.id = "keybind";
    keybindNode.key = "keybind";
    keybindNode.title = "Keybind";
    keybindNode.category = "combat";
    keybindNode.type = ConfigControlTypeV2::Keybind;
    schema.node(std::move(keybindNode));

    const auto selected = std::find_if(profiles.begin(), profiles.end(),
        [&](const auto& profile) { return profile.identifier == selectedIdentifier; });
    if (selected != profiles.end()) {
        ConfigNodeV2 settingsSection;
        settingsSection.id = "m_selected_profile_section";
        settingsSection.title = selected->displayName + " — Profile Settings";
        settingsSection.description = "Exact item ID: " + selected->identifier +
            ". Initial Burst counts accepted Key Mapper broadcast requests, not server-confirmed damage hits.";
        settingsSection.category = "profile_settings";
        settingsSection.type = ConfigControlTypeV2::Section;
        schema.node(std::move(settingsSection));

        auto addProfileSlider = [&](const char* key, const char* title,
                                    const char* minimum, const char* maximum,
                                    const char* step, const char* unit,
                                    std::string current, std::string description) {
            ConfigNodeV2 node;
            node.id = key;
            node.key = key;
            node.title = title;
            node.description = std::move(description);
            node.category = "profile_settings";
            node.type = ConfigControlTypeV2::SliderFloat;
            node.defaultValue = current;
            node.currentValue = std::move(current);
            node.hasCurrentValue = true;
            node.minValue = minimum;
            node.maxValue = maximum;
            node.step = step;
            node.unit = unit;
            schema.node(std::move(node));
        };
        addProfileSlider("m_selected_profile_range", "Attack range", "1", "6", "0.1",
                         " blocks", std::to_string(selected->range),
                         "This item's range is additionally limited by the current Aim Assist tracking range.");
        addProfileSlider("m_selected_profile_cps", "Normal CPS", "1", "12", "0.5",
                         " CPS", std::to_string(selected->cps),
                         "Steady request rate after the initial burst ends.");
        ConfigNodeV2 burstHits;
        burstHits.id = "m_selected_profile_burst_hits";
        burstHits.key = burstHits.id;
        burstHits.title = "Initial Burst Hits (0 = off)";
        burstHits.description = "Number of accepted input requests allowed in the fast opening burst when the item or target changes.";
        burstHits.category = "profile_settings";
        burstHits.type = ConfigControlTypeV2::SliderInt;
        burstHits.defaultValue = "0";
        burstHits.currentValue = std::to_string(selected->initialBurstHits);
        burstHits.hasCurrentValue = true;
        burstHits.minValue = "0";
        burstHits.maxValue = "20";
        burstHits.step = "1";
        schema.node(std::move(burstHits));
        addProfileSlider("m_selected_profile_burst_cps", "Initial Burst CPS", "1", "20", "0.5",
                         " CPS", std::to_string(selected->initialBurstCps),
                         "Only applies to the configured first N accepted requests; then Normal CPS takes over.");
        ConfigNodeV2 backButton;
        backButton.id = "m_profile_back";
        backButton.key = backButton.id;
        backButton.title = "Close profile settings";
        backButton.description = "Return focus to the weapon profile list.";
        backButton.category = "profile_settings";
        backButton.type = ConfigControlTypeV2::Button;
        backButton.actionValue = "back";
        schema.node(std::move(backButton));
    } else {
        ConfigNodeV2 info;
        info.id = "m_profile_settings_info";
        info.title = "Select Settings on a weapon to edit its profile.";
        info.category = "profile_settings";
        info.type = ConfigControlTypeV2::Info;
        schema.node(std::move(info));
    }

    setConfigSchemaJson(moduleId, schema.toJson());
}

bool AutoHitModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "keybind") return false;

    if (key == "m_addWeaponChoice") {
        const std::string choice = bedaim::autohit::weapon::normalizeIdentifier(value);
        const bool known = std::any_of(
            bedaim::autohit::weapon::kWeaponCatalog.begin(),
            bedaim::autohit::weapon::kWeaponCatalog.end(),
            [&](const auto& entry) { return entry.identifier == choice; });
        if (!known) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_addWeaponChoice = choice;
        return true;
    }
    if (key == "m_customWeaponIdentifier") {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_customWeaponIdentifier = std::string(value.substr(0, 128));
        return true;
    }

    if (key == "m_addWeaponButton") {
        bool schemaNeedsRefresh = false;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const std::string requested = m_customWeaponIdentifier.empty()
                ? m_addWeaponChoice : m_customWeaponIdentifier;
            const std::string identifier = bedaim::autohit::weapon::normalizeIdentifier(requested);
            if (identifier.empty() || identifier.size() > 128) return false;
            const auto existing = std::find_if(m_customWeaponProfiles.begin(), m_customWeaponProfiles.end(),
                [&](const auto& profile) { return profile.identifier == identifier; });
            if (existing != m_customWeaponProfiles.end()) {
                m_selectedProfileIdentifier = existing->identifier;
                schemaNeedsRefresh = true;
            } else {
                if (m_customWeaponProfiles.size() >= 120) return false;
                const auto catalogEntry = std::find_if(
                    bedaim::autohit::weapon::kWeaponCatalog.begin(),
                    bedaim::autohit::weapon::kWeaponCatalog.end(),
                    [&](const auto& entry) { return entry.identifier == identifier; });
                WeaponCustomProfile profile;
                if (catalogEntry != bedaim::autohit::weapon::kWeaponCatalog.end()) {
                    profile = makeLegacyProfile(
                        *catalogEntry, m_attackRange,
                        m_swordEnabled, m_swordCps,
                        m_axeEnabled, m_axeCps,
                        m_maceEnabled, m_maceCps,
                        m_tridentEnabled, m_tridentCps);
                } else {
                    profile.identifier = identifier;
                    profile.displayName = bedaim::autohit::weapon::displayNameForIdentifier(identifier);
                    profile.enabled = false;
                    profile.range = m_attackRange;
                    const auto type = bedaim::autohit::weapon::classify(identifier);
                    switch (type) {
                        case bedaim::autohit::weapon::Class::Sword: profile.cps = m_swordCps; break;
                        case bedaim::autohit::weapon::Class::Axe: profile.cps = m_axeCps; break;
                        case bedaim::autohit::weapon::Class::Mace: profile.cps = m_maceCps; break;
                        case bedaim::autohit::weapon::Class::Trident: profile.cps = m_tridentCps; break;
                        case bedaim::autohit::weapon::Class::Spear: profile.cps = 3.0f; break;
                        case bedaim::autohit::weapon::Class::Bow:
                        case bedaim::autohit::weapon::Class::Crossbow: profile.cps = 4.0f; break;
                        case bedaim::autohit::weapon::Class::Unsupported: profile.cps = 4.0f; break;
                    }
                    profile.initialBurstHits = 0;
                    profile.initialBurstCps = 13.0f;
                    profile = bedaim::autohit::weapon::normalizeCustomProfile(std::move(profile));
                }
                m_customWeaponProfiles.push_back(std::move(profile));
                m_selectedProfileIdentifier = identifier;
                m_customWeaponIdentifier.clear();
                m_burstWeaponIdentifier.clear();
                m_burstTarget = nullptr;
                m_burstRemaining = 0;
                m_nextAttack = {};
                schemaNeedsRefresh = true;
            }
        }
        if (schemaNeedsRefresh) onMenuRegistered();
        return schemaNeedsRefresh;
    }

    if (key == "m_profile_back") {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_selectedProfileIdentifier.clear();
        }
        onMenuRegistered();
        return true;
    }

    std::size_t index = 0;
    if (parseIndexSuffix(key, "m_profile_settings_", index)) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (index >= m_customWeaponProfiles.size()) return false;
            m_selectedProfileIdentifier = m_customWeaponProfiles[index].identifier;
        }
        onMenuRegistered();
        return true;
    }
    if (parseIndexSuffix(key, "m_profile_delete_", index)) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (index >= m_customWeaponProfiles.size()) return false;
            const std::string removed = m_customWeaponProfiles[index].identifier;
            m_customWeaponProfiles.erase(m_customWeaponProfiles.begin() + static_cast<std::ptrdiff_t>(index));
            if (m_selectedProfileIdentifier == removed)
                m_selectedProfileIdentifier = m_customWeaponProfiles.empty()
                    ? std::string{} : m_customWeaponProfiles.front().identifier;
            if (m_burstWeaponIdentifier == removed) {
                m_burstWeaponIdentifier.clear();
                m_burstTarget = nullptr;
                m_burstRemaining = 0;
            }
            m_nextAttack = {};
        }
        onMenuRegistered();
        return true;
    }
    if (parseIndexSuffix(key, "m_profile_enabled_", index)) {
        bool enabledValue = false;
        if (!parseBoolValue(value, enabledValue)) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (index >= m_customWeaponProfiles.size()) return false;
        m_customWeaponProfiles[index].enabled = enabledValue;
        return true;
    }

    if (key == "m_attackRange") {
        try {
            const std::string raw(value);
            std::size_t parsed = 0;
            const float number = std::stof(raw, &parsed);
            while (parsed < raw.size() && std::isspace(static_cast<unsigned char>(raw[parsed]))) ++parsed;
            if (parsed != raw.size() || !std::isfinite(number)) return false;
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_attackRange = std::clamp(number, 2.0f, 6.0f);
            return true;
        } catch (...) { return false; }
    }

    if (key == "m_selected_profile_range" || key == "m_selected_profile_cps" ||
        key == "m_selected_profile_burst_hits" || key == "m_selected_profile_burst_cps") {
        try {
            const std::string raw(value);
            std::size_t parsed = 0;
            const float number = std::stof(raw, &parsed);
            while (parsed < raw.size() && std::isspace(static_cast<unsigned char>(raw[parsed]))) ++parsed;
            if (parsed != raw.size() || !std::isfinite(number)) return false;

            std::lock_guard<std::mutex> lock(m_stateMutex);
            const auto profile = std::find_if(m_customWeaponProfiles.begin(), m_customWeaponProfiles.end(),
                [&](const auto& item) { return item.identifier == m_selectedProfileIdentifier; });
            if (profile == m_customWeaponProfiles.end()) return false;
            if (key == "m_selected_profile_range")
                profile->range = bedaim::autohit::weapon::normalizeRange(number);
            else if (key == "m_selected_profile_cps")
                profile->cps = bedaim::autohit::weapon::normalizeNormalCps(number);
            else if (key == "m_selected_profile_burst_hits")
                profile->initialBurstHits = bedaim::autohit::weapon::normalizeBurstHits(
                    static_cast<int>(std::lround(number)));
            else
                profile->initialBurstCps = bedaim::autohit::weapon::normalizeBurstCps(number);
            return true;
        } catch (...) { return false; }
    }

    return false;
}
