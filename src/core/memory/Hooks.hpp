#pragma once

#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <mutex>
#include <new>
#include <pl/memory/Hook.hpp>

namespace bedrocktools::hooks {

using LibraryHandle = void*;

enum class Backend : std::uint8_t {
    Uninitialized,
    ShadowHook,
    Preloader
};

struct State {
    void* target;
    void* detour;
    void* shadowhookStub;
    Backend backend;
};

using Handle = State*;

namespace detail {
using ShadowHookInitFn = int (*)(int, bool);
using ShadowHookHookFn = void* (*)(void*, void*, void**);
using ShadowHookUnhookFn = int (*)(void*);

inline std::once_flag backendInitOnce;
inline Backend activeBackend = Backend::Uninitialized;
inline void* shadowhookLibrary = nullptr;
inline ShadowHookHookFn shadowhookHook = nullptr;
inline ShadowHookUnhookFn shadowhookUnhook = nullptr;

inline void initializeBackend() noexcept {
    std::call_once(backendInitOnce, [] {
#if defined(__ANDROID__)
        // Prefer an already-loaded copy (e.g. one supplied by the host client),
        // but allow Android's linker to load a packaged libshadowhook.so too.
        void* library = dlopen("libshadowhook.so", RTLD_NOW | RTLD_NOLOAD);
        if (!library) library = dlopen("libshadowhook.so", RTLD_NOW);
        if (library) {
            auto init = reinterpret_cast<ShadowHookInitFn>(
                dlsym(library, "shadowhook_init"));
            auto hook = reinterpret_cast<ShadowHookHookFn>(
                dlsym(library, "shadowhook_hook_func_addr"));
            auto unhook = reinterpret_cast<ShadowHookUnhookFn>(
                dlsym(library, "shadowhook_unhook"));

            // ShadowHook's native API uses mode 0 for shared mode. Keep the
            // library loaded for the lifetime of its stubs.
            if (init && hook && unhook && init(0, false) == 0) {
                shadowhookLibrary = library;
                shadowhookHook = hook;
                shadowhookUnhook = unhook;
                activeBackend = Backend::ShadowHook;
                return;
            }
            dlclose(library);
        }
#endif
        // Compatibility path for launchers that do not provide ShadowHook.
        // Backend selection is process-wide for this module: do not mix
        // backends per hook after one has been selected.
        activeBackend = Backend::Preloader;
    });
}
} // namespace detail

inline LibraryHandle openLibrary(const char* libraryName) {
    if (!libraryName) return nullptr;
    void* handle = dlopen(libraryName, RTLD_NOW | RTLD_NOLOAD);
    return handle ? handle : dlopen(libraryName, RTLD_NOW);
}

inline int closeLibrary(LibraryHandle handle) {
    return handle ? dlclose(handle) : 0;
}

inline std::uintptr_t symbol(LibraryHandle handle, const char* name) {
    if (!handle || !name) return 0;
    return reinterpret_cast<std::uintptr_t>(dlsym(handle, name));
}

inline Backend backend() noexcept {
    detail::initializeBackend();
    return detail::activeBackend;
}

inline const char* backendName() noexcept {
    switch (backend()) {
        case Backend::ShadowHook: return "ShadowHook";
        case Backend::Preloader: return "Preloader";
        case Backend::Uninitialized: default: return "uninitialized";
    }
}

inline Handle install(void* target, void* detour, void** original) {
    if (!target || !detour) return nullptr;
    detail::initializeBackend();

    auto* state = new (std::nothrow) State{target, detour, nullptr, detail::activeBackend};
    if (!state) return nullptr;

    if (state->backend == Backend::ShadowHook) {
        state->shadowhookStub = detail::shadowhookHook(target, detour, original);
        if (!state->shadowhookStub) {
            delete state;
            return nullptr;
        }
        return state;
    }

    if (pl::memory::hook(target, detour, original) != 0) {
        delete state;
        return nullptr;
    }
    return state;
}

inline void remove(Handle hook) {
    if (!hook) return;
    if (hook->backend == Backend::ShadowHook) {
        if (hook->shadowhookStub && detail::shadowhookUnhook) {
            detail::shadowhookUnhook(hook->shadowhookStub);
        }
    } else if (hook->backend == Backend::Preloader) {
        pl::memory::unhook(hook->target, hook->detour);
    }
    delete hook;
}

} // namespace bedrocktools::hooks
