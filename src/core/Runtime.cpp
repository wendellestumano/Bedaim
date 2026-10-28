#include "Runtime.hpp"
#include "JavaVm.hpp"
#include "GameHooks.hpp"
#include "config/ConfigManager.hpp"
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <filesystem>
#include <fstream>
#include <array>
#include "launcher/ModuleMenu.hpp"
#include "modules/ModuleRegistry.hpp"
#include "core/memory/Hooks.hpp"
#include "core/Diagnostics.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <pl/Input.hpp>
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace bedrocktools::core {
namespace {
std::atomic_bool enabled = false;
std::atomic_bool resolved = false;
std::atomic_bool installed = false;
std::mutex resolveMutex;
std::mutex installMutex;
thread_local bool resolvingFromDlopen = false;
void* (*dlopenOriginal)(const char*, int) = nullptr;
bedrocktools::hooks::Handle dlopenHook = nullptr;
bool eventsWired = false;
int containerDepth = 0;
int chatDepth = 0;
std::atomic<std::uintptr_t> lastWorldIdentity{0};

class ResolveGuard {
public:
    ResolveGuard() : mPrevious(resolvingFromDlopen) { resolvingFromDlopen = true; }
    ~ResolveGuard() { resolvingFromDlopen = mPrevious; }
private:
    bool mPrevious;
};

void* dlopenDetour(const char* filename, int flags) {
    void* handle = dlopenOriginal ? dlopenOriginal(filename, flags) : nullptr;
    if (handle && filename && std::strstr(filename, "libminecraftpe.so") && !resolvingFromDlopen) {
        Runtime::get().minecraftLoaded();
    }
    return handle;
}
}

Runtime& Runtime::get() {
    static Runtime runtime;
    return runtime;
}

const std::filesystem::path& Runtime::resourceDirectory() const noexcept {
    return mResourceDirectory;
}

bool Runtime::launcherContext() const {
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return false;
    char command[256]{};
    const auto size = read(fd, command, sizeof(command) - 1);
    close(fd);
    if (size <= 0) return false;
    return std::strcmp(command, "org.levimc.launcher") == 0
        || std::strcmp(command, "org.levimc.launcher:minecraft") == 0
        || std::strcmp(command, "com.mojang.minecraftpe") == 0;
}

bool Runtime::resolveSignatures() {
    std::lock_guard lock(resolveMutex);
    if (resolved.load(std::memory_order_acquire)) return true;
    ResolveGuard guard;
    const bool ok = bedrocktools::memory::resolveAll("libminecraftpe.so");
    resolved.store(ok, std::memory_order_release);
    bedrocktools::diagnostics::log(ok ? bedrocktools::diagnostics::Level::Info : bedrocktools::diagnostics::Level::Error,
        "Runtime", ok ? "Minecraft signatures resolved" : "Minecraft signature resolution failed");
    return ok;
}

void Runtime::wireEvents() {
    if (eventsWired) return;
    eventsWired = true;
    using namespace bedrocktools::events;
    bus().subscribe<FrameEvent>([](auto&) { ModuleRegistry::get().onFrame(); });
    bus().subscribe<LocalPlayerTickEvent>([](auto& event) {
        if (!event.player) return;
        auto* actor = static_cast<bedrocktools::sdk::Actor*>(event.player);
        const auto worldIdentity = reinterpret_cast<std::uintptr_t>(actor->level());
        if (worldIdentity == 0) return;
        const auto previous = lastWorldIdentity.exchange(worldIdentity, std::memory_order_acq_rel);
        if (previous != worldIdentity) {
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Info, "Config",
                "world identity changed; reloading Downloads/BEDAIM/config.json");
            bedrocktools::config::ConfigManager::get().load();
        }
    });
    bus().subscribe<MouseInputEvent>([](auto& event) {
        if (ModuleRegistry::get().onMouseEvent(event.button, event.down)) event.cancel();
    });
    bus().subscribe<ScreenStateEvent>([](auto& event) {
        int& depth = event.screen == ScreenKind::Container ? containerDepth : chatDepth;
        if (event.phase == ScreenPhase::Opened) ++depth;
        else if (depth > 0) --depth;
        ModuleRegistry::get().setKeybindBlocked(containerDepth > 0 || chatDepth > 0);
    });
    pl::input::registerMouseCallback([](const pl::input::MouseEvent& input) {
        MouseInputEvent event{input.button, input.isDown};
        bus().publish(event);
        return event.cancelled();
    });
}

bool Runtime::install() {
    std::lock_guard lock(installMutex);
    if (installed.load(std::memory_order_acquire)) return true;
    if (!resolved.load(std::memory_order_acquire) && !resolveSignatures()) return false;
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Runtime", "installing game hooks and modules");
    if (!gamehooks::install()) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error, "Runtime", "game hook installation failed");
        return false;
    }
    registerAllModules();
    wireEvents();
    ModuleRegistry::get().initialize();
    bedrocktools::config::ConfigManager::get().load();
    registerModulesWithLauncher();
    installed.store(true, std::memory_order_release);
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Runtime", "runtime installed and launcher modules registered");
    return true;
}

void Runtime::minecraftLoaded() {
    if (!resolveSignatures()) return;
    if (enabled.load(std::memory_order_acquire)) install();
}

bool Runtime::load(pl::mod::ModContext& context) {
    // The official Preloader exposes the correct process JavaVM here. Game-tick
    // callbacks run after load() returns, so cache it instead of relying only on
    // JNI_GetCreatedJavaVMs, which may be hidden by Android linker namespaces.
    setJavaVm(context.javaVm());
    bedrocktools::diagnostics::log(
        context.javaVm() ? bedrocktools::diagnostics::Level::Info
                         : bedrocktools::diagnostics::Level::Warning,
        "Runtime",
        context.javaVm() ? "JavaVM captured from Preloader ModContext"
                         : "Preloader ModContext provided a null JavaVM");
    mResourceDirectory = context.resourceDir();
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Runtime", "Bedaim load requested");
    // Android's public Downloads directory is normally exposed as "Download"
    // on disk. Prefer any existing config so upgrades do not silently switch files.
    const std::filesystem::path pluralDownloads("/storage/emulated/0/Downloads/BEDAIM/config.json");
    const std::filesystem::path sdcardPluralDownloads("/sdcard/Downloads/BEDAIM/config.json");
    const std::filesystem::path conventionalDownload("/storage/emulated/0/Download/BEDAIM/config.json");
    const std::filesystem::path sdcardDownload("/sdcard/Download/BEDAIM/config.json");
    std::error_code configPathError;
    const std::array<std::filesystem::path, 4> publicConfigCandidates{
        pluralDownloads, sdcardPluralDownloads, conventionalDownload, sdcardDownload
    };
    std::filesystem::path configPath = conventionalDownload;
    bool foundExistingConfig = false;
    for (const auto& candidate : publicConfigCandidates) {
        configPathError.clear();
        if (std::filesystem::exists(candidate, configPathError) && !configPathError) {
            configPath = candidate;
            foundExistingConfig = true;
            break;
        }
    }
    // If no config exists yet, respect a BEDAIM folder the user already created
    // under Downloads before defaulting to Android's standard Download directory.
    if (!foundExistingConfig) {
        for (const auto& candidate : publicConfigCandidates) {
            configPathError.clear();
            if (std::filesystem::is_directory(candidate.parent_path(), configPathError) &&
                !configPathError) {
                configPath = candidate;
                break;
            }
        }
    }

    // Confirm the selected public folder is actually writable by the game process.
    // Scoped-storage restrictions can make Downloads inaccessible even when the path
    // exists; in that case use the launcher's private config directory rather than
    // silently losing every save. Keep the selected path in diagnostics.
    auto canWriteBesideConfig = [](const std::filesystem::path& config) {
        std::error_code ec;
        std::filesystem::create_directories(config.parent_path(), ec);
        if (ec) return false;
        const auto probe = config.parent_path() / ".bedaim_write_test.tmp";
        {
            std::ofstream out(probe, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << "ok";
            out.flush();
            if (!out.good()) {
                out.close();
                std::filesystem::remove(probe, ec);
                return false;
            }
        }
        std::filesystem::remove(probe, ec);
        return !ec;
    };
    if (!canWriteBesideConfig(configPath)) {
        const auto privateConfig = context.configDir() / "BEDAIM" / "config.json";
        if (canWriteBesideConfig(privateConfig)) {
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning, "Config",
                "Downloads is not writable by the game process; using launcher's private config directory");
            configPath = privateConfig;
        } else {
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Error, "Config",
                "neither Downloads nor launcher's private config directory is writable");
        }
    }
    bedrocktools::config::ConfigManager::get().setConfigPath(configPath.string());
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Config",
        "active config path: " + configPath.string());
    if (!launcherContext()) return true;
    void* minecraft = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (minecraft) {
        resolveSignatures();
        dlclose(minecraft);
        return true;
    }
    bedrocktools::hooks::LibraryHandle libdl = bedrocktools::hooks::openLibrary("libdl.so");
    if (!libdl) return true;
    void* symbol = reinterpret_cast<void*>(bedrocktools::hooks::symbol(libdl, "dlopen"));
    if (symbol) dlopenHook = bedrocktools::hooks::install(symbol, reinterpret_cast<void*>(dlopenDetour), reinterpret_cast<void**>(&dlopenOriginal));
    bedrocktools::hooks::closeLibrary(libdl);
    return true;
}

bool Runtime::enable(pl::mod::ModContext&) {
    enabled.store(true, std::memory_order_release);
    if (!launcherContext()) return true;
    if (!resolved.load(std::memory_order_acquire)) {
        void* minecraft = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
        if (!minecraft) return true;
        resolveSignatures();
        dlclose(minecraft);
    }
    install();
    return true;
}

bool Runtime::disable(pl::mod::ModContext&) {
    enabled.store(false, std::memory_order_release);
    bedrocktools::config::ConfigManager::get().flush();
    return true;
}

bool Runtime::unload(pl::mod::ModContext&) {
    enabled.store(false, std::memory_order_release);
    bedrocktools::config::ConfigManager::get().flush();
    return true;
}

}
