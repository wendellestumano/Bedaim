#include "seedmap.hpp"

#include "../../core/Diagnostics.hpp"
#include "../../core/JavaVm.hpp"
#include "../../core/memory/Hooks.hpp"
#include "../ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <pl/ModMenuConfig.hpp>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace {
using HandleTextFn = void (*)(void*, const bedrocktools::sdk::TextPacket&);

std::atomic<SeedMapModule*> g_seedMapModule{nullptr};
void* g_handleTextOriginal = nullptr;
bedrocktools::hooks::Handle g_handleTextHook = nullptr;
std::mutex g_handleTextHookMutex;

void handleTextDetour(void* handler, const bedrocktools::sdk::TextPacket& packet) {
    if (auto* module = g_seedMapModule.load(std::memory_order_acquire))
        module->observeTextPacket(packet);

    const auto original = reinterpret_cast<HandleTextFn>(g_handleTextOriginal);
    if (original) original(handler, packet);
}

std::string trimCopy(std::string_view input) {
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
        input.remove_prefix(1);
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
        input.remove_suffix(1);
    return std::string(input);
}

void clearJniException(JNIEnv* env) {
    if (env && env->ExceptionCheck()) env->ExceptionClear();
}

} // namespace

SeedMapModule::SeedMapModule()
    : Module("SeedMap", "Open the current Bedrock world in Chunkbase Seed Map, using the captured seed and live player coordinates.") {
    showInMenu = true;
}

void SeedMapModule::onInit() {
    g_seedMapModule.store(this, std::memory_order_release);
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { onPlayerTick(event.player); });

    if (installTextHook()) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "SeedMap",
            "seed_capture=ready: listening for incoming chat messages containing a numeric seed announcement");
    } else {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning, "SeedMap",
            "seed_capture=unavailable: manual seed entry remains available");
    }
}

bool SeedMapModule::installTextHook() {
    std::lock_guard<std::mutex> lock(g_handleTextHookMutex);
    if (g_handleTextHook) return true;

    const auto target = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::ClientNetworkHandlerHandleText);
    if (!target) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "SeedMap", "chat_seed_hook", "signature_unresolved",
            "ClientNetworkHandlerHandleText was not resolved for this runtime");
        return false;
    }

    g_handleTextHook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(&handleTextDetour),
        &g_handleTextOriginal);
    if (!g_handleTextHook) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Error,
            "SeedMap", "chat_seed_hook", "install_failed");
        return false;
    }
    return true;
}

std::string SeedMapModule::parseSeedAnnouncement(std::string_view message) {
    std::string lower(message);
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    const auto marker = lower.find("seed");
    if (marker == std::string::npos) return {};

    std::size_t cursor = marker + 4;
    while (cursor < message.size() &&
           !(std::isdigit(static_cast<unsigned char>(message[cursor])) ||
             message[cursor] == '-' || message[cursor] == '+')) {
        ++cursor;
    }
    if (cursor >= message.size()) return {};

    const std::size_t start = cursor;
    if (message[cursor] == '-' || message[cursor] == '+') ++cursor;
    const std::size_t digitsStart = cursor;
    while (cursor < message.size() &&
           std::isdigit(static_cast<unsigned char>(message[cursor]))) {
        ++cursor;
    }
    if (cursor == digitsStart) return {};

    const auto candidate = message.substr(start, cursor - start);
    std::int64_t parsed = 0;
    const char* parseBegin = candidate.data();
    if (!candidate.empty() && candidate.front() == '+') ++parseBegin;
    const auto result = std::from_chars(parseBegin,
        candidate.data() + candidate.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != candidate.data() + candidate.size())
        return {};
    return std::string(candidate);
}

void SeedMapModule::observeTextPacket(const bedrocktools::sdk::TextPacket& packet) {
    if (!enabled) return;

    std::string detected;
    try {
        detected = parseSeedAnnouncement(packet.message());
    } catch (...) {
        return;
    }
    if (detected.empty()) return;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_seed != detected) {
            m_seed = detected;
            changed = true;
        }
    }
    if (changed) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "SeedMap",
            std::string("seed_capture=success: captured numeric seed from incoming chat: ") + detected);
    }
}

void SeedMapModule::onPlayerTick(bedrocktools::sdk::Player* player) {
    if (!player) return;
    const auto position = player->position();
    if (!std::isfinite(position.x) || !std::isfinite(position.z)) return;

    const auto clampCoordinate = [](float value) {
        const double floored = std::floor(static_cast<double>(value));
        return static_cast<int>(std::clamp(floored,
            static_cast<double>(std::numeric_limits<int>::min()),
            static_cast<double>(std::numeric_limits<int>::max())));
    };

    int dimensionId = -1;
    if (auto* dimension = player->dimension()) {
        if (auto* source = dimension->blockSource())
            dimensionId = source->dimensionId();
    }

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_playerX = clampCoordinate(position.x);
    m_playerZ = clampCoordinate(position.z);
    m_hasPlayerPosition = true;

    if (dimensionId == 0) m_dimension = "overworld";
    else if (dimensionId == 1) m_dimension = "nether";
    else if (dimensionId == 2) m_dimension = "end";
}

std::string SeedMapModule::urlEncode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (const unsigned char ch : value) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (safe) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[(ch >> 4) & 0x0F]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }
    return encoded;
}

bool SeedMapModule::openUrlInBrowser(std::string_view url) {
    JavaVM* vm = bedrocktools::core::javaVm();
    if (!vm) return false;

    JNIEnv* env = nullptr;
    bool attachedHere = false;
    const jint getEnvResult = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvResult == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attachedHere = true;
    } else if (getEnvResult != JNI_OK || !env) {
        return false;
    }

    bool success = false;
    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if (activityThread && !env->ExceptionCheck()) {
        const jmethodID currentApplication = env->GetStaticMethodID(activityThread,
            "currentApplication", "()Landroid/app/Application;");
        if (currentApplication && !env->ExceptionCheck()) {
            jobject application = env->CallStaticObjectMethod(activityThread, currentApplication);
            if (application && !env->ExceptionCheck()) {
                jclass intentClass = env->FindClass("android/content/Intent");
                jclass uriClass = env->FindClass("android/net/Uri");
                jclass contextClass = env->FindClass("android/content/Context");
                if (intentClass && uriClass && contextClass && !env->ExceptionCheck()) {
                    const jmethodID intentCtor = env->GetMethodID(intentClass, "<init>", "(Ljava/lang/String;)V");
                    const jmethodID parseUri = env->GetStaticMethodID(uriClass, "parse",
                        "(Ljava/lang/String;)Landroid/net/Uri;");
                    const jmethodID setData = env->GetMethodID(intentClass, "setData",
                        "(Landroid/net/Uri;)Landroid/content/Intent;");
                    const jmethodID addFlags = env->GetMethodID(intentClass, "addFlags",
                        "(I)Landroid/content/Intent;");
                    const jmethodID startActivity = env->GetMethodID(contextClass, "startActivity",
                        "(Landroid/content/Intent;)V");

                    if (intentCtor && parseUri && setData && addFlags && startActivity &&
                        !env->ExceptionCheck()) {
                        jstring action = env->NewStringUTF("android.intent.action.VIEW");
                        jstring urlString = env->NewStringUTF(std::string(url).c_str());
                        jobject intent = action ? env->NewObject(intentClass, intentCtor, action) : nullptr;
                        jobject uri = urlString ? env->CallStaticObjectMethod(uriClass, parseUri, urlString) : nullptr;
                        if (intent && uri && !env->ExceptionCheck()) {
                            env->CallObjectMethod(intent, setData, uri);
                            env->CallObjectMethod(intent, addFlags, static_cast<jint>(0x10000000));
                            if (!env->ExceptionCheck()) {
                                env->CallVoidMethod(application, startActivity, intent);
                                success = !env->ExceptionCheck();
                            }
                        }
                        if (intent) env->DeleteLocalRef(intent);
                        if (uri) env->DeleteLocalRef(uri);
                        if (action) env->DeleteLocalRef(action);
                        if (urlString) env->DeleteLocalRef(urlString);
                    }
                }
                if (intentClass) env->DeleteLocalRef(intentClass);
                if (uriClass) env->DeleteLocalRef(uriClass);
                if (contextClass) env->DeleteLocalRef(contextClass);
                env->DeleteLocalRef(application);
            }
        }
    }

    clearJniException(env);
    if (activityThread) env->DeleteLocalRef(activityThread);
    if (attachedHere) vm->DetachCurrentThread();
    return success;
}

bool SeedMapModule::openSeedMap() {
    std::string seed;
    std::string platform;
    std::string dimension;
    int x = 0;
    int z = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        seed = m_seed;
        platform = m_platform;
        dimension = m_dimension;
        if (m_hasPlayerPosition) {
            x = m_playerX;
            z = m_playerZ;
        }
    }

    seed = trimCopy(seed);
    if (seed.empty()) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning, "SeedMap",
            "open_blocked=no_seed: enter the world seed or wait for a numeric seed announcement in chat");
        return false;
    }

    std::string url = "https://www.chunkbase.com/apps/seed-map#seed=" + urlEncode(seed);
    if (!platform.empty()) url += "&platform=" + urlEncode(platform);
    url += "&dimension=" + urlEncode(dimension);
    url += "&x=" + std::to_string(x);
    url += "&z=" + std::to_string(z);

    const bool opened = openUrlInBrowser(url);
    bedrocktools::diagnostics::log(opened ? bedrocktools::diagnostics::Level::Info
                                          : bedrocktools::diagnostics::Level::Error,
        "SeedMap", opened ? "browser_open=success" : "browser_open=failed: Android application context or Intent unavailable");
    return opened;
}

void SeedMapModule::onMenuRegistered() {
    using namespace pl::modmenu;
    std::string seedValue;
    std::string platformValue;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        seedValue = m_seed;
        platformValue = m_platform;
    }
    ConfigSchemaBuilder schema;
    schema.defaultCategory("world")
        .category("world", "World seed", "Capture or enter the seed, then open the map at your current coordinates.")
        .category("map", "Map version", "Optional Chunkbase platform identifier. Leave blank to choose the version in the browser.");

    ConfigNodeV2 seed;
    seed.id = "seed";
    seed.key = "seed";
    seed.category = "world";
    seed.title = "World seed";
    seed.description = "Numeric or text seed. Incoming chat messages containing 'seed' followed by a 64-bit integer are captured while this module is enabled.";
    seed.type = ConfigControlTypeV2::Text;
    seed.defaultValue = "";
    seed.currentValue = seedValue;
    seed.hasCurrentValue = true;
    seed.placeholder = "Enter world seed";
    seed.maxLength = 32;
    schema.node(std::move(seed));

    ConfigNodeV2 open;
    open.id = "openSeedMap";
    open.key = "openSeedMap";
    open.category = "world";
    open.title = "Open SeedMap";
    open.description = "Open Chunkbase Seed Map with the saved seed, current dimension and player X/Z.";
    open.type = ConfigControlTypeV2::Button;
    open.actionValue = "open";
    schema.node(std::move(open));

    ConfigNodeV2 platform;
    platform.id = "platform";
    platform.key = "platform";
    platform.category = "map";
    platform.title = "Platform identifier";
    platform.description = "Optional Chunkbase platform/version identifier, for example bedrock_1_21_120. Leave blank to select the version manually.";
    platform.type = ConfigControlTypeV2::Text;
    platform.defaultValue = "";
    platform.currentValue = platformValue;
    platform.hasCurrentValue = true;
    platform.placeholder = "bedrock_...";
    platform.maxLength = 40;
    schema.node(std::move(platform));

    ConfigNodeV2 info;
    info.id = "seedCaptureInfo";
    info.key = "seedCaptureInfo";
    info.category = "world";
    info.title = "Automatic seed capture";
    info.description = "The module listens for incoming chat messages that explicitly contain 'seed' and a numeric value. If a multiplayer server does not send the seed to clients, enter it manually; this module does not guess hidden server data.";
    info.type = ConfigControlTypeV2::Info;
    schema.node(std::move(info));

    setConfigSchemaJson(moduleId, schema.toJson());
}

bool SeedMapModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "openSeedMap") {
        openSeedMap();
        return true;
    }
    if (key == "seed") {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_seed = trimCopy(value);
        return true;
    }
    if (key == "platform") {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_platform = trimCopy(value);
        return true;
    }
    return false;
}

void SeedMapModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_seed = json.value("seed", m_seed);
    m_platform = json.value("platform", m_platform);
}

void SeedMapModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    json["seed"] = m_seed;
    json["platform"] = m_platform;
}
