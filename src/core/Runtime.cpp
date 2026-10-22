#include "Runtime.hpp"
#include "JavaVm.hpp"
#include "GameHooks.hpp"
#include "config/ConfigManager.hpp"
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
    // Use the JavaVM that the official Preloader already passes to the mod.
    // This remains valid after load() returns, unlike NativeMod::current(),
    // which is only scoped to Preloader lifecycle callbacks.
    bedrocktools::core::setJavaVm(context.javaVm());
    bedrocktools::diagnostics::log(
        context.javaVm() ? bedrocktools::diagnostics::Level::Info
                         : bedrocktools::diagnostics::Level::Warning,
        "Runtime",
        context.javaVm() ? "JavaVM captured from Preloader ModContext"
                         : "Preloader ModContext provided a null JavaVM");
    mResourceDirectory = context.resourceDir();
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, "Runtime", "Bedaim load requested");
    bedrocktools::config::ConfigManager::get().setConfigPath((context.configDir() / "config.json").string());
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
