#include "ModuleRegistry.hpp"
#include "../core/Diagnostics.hpp"
#include "player/aimassist.hpp"
#include "player/autohit.hpp"
#include "player/maceswapper.hpp"
#include "player/unglide.hpp"
#include "player/swordswapper.hpp"
#include "hud/ultrakillhud.hpp"
#include "visual/connectedglass.hpp"
#include "misc/modulediagnostics.hpp"
#include "visual/esp.hpp"

ModuleRegistry& ModuleRegistry::get() {
    static ModuleRegistry registry;
    return registry;
}

Module* ModuleRegistry::find(std::string_view id) const {
    const auto it = mById.find(id);
    return it == mById.end() ? nullptr : it->second;
}

const std::vector<Module*>& ModuleRegistry::modules() const {
    return mView;
}

void ModuleRegistry::initialize() {
    if (mInitialized) return;
    for (auto* module : mView) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Debug,
            "ModuleRegistry", std::string("initializing module: ") + module->moduleId);
        module->onInit();
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Trace,
            "ModuleRegistry", std::string("module initialized: ") + module->moduleId);
    }
    mInitialized = true;
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info,
        "ModuleRegistry", std::string("registered modules initialized: ") + std::to_string(mView.size()));
}

void ModuleRegistry::onFrame() {
    for (auto* module : mView) if (module->enabled) module->onFrame();
}

bool ModuleRegistry::onMouseEvent(int button, bool isDown) {
    bool consumed = false;
    for (auto* module : mView) if (module->onMouseEvent(button, isDown)) consumed = true;
    return consumed;
}

void ModuleRegistry::setKeybindBlocked(bool blocked) {
    mKeybindBlocked = blocked;
}

bool ModuleRegistry::keybindBlocked() const {
    return mKeybindBlocked;
}

void registerAllModules() {
    auto& registry = ModuleRegistry::get();
    if (!registry.modules().empty()) return;

    // Bedaim-owned modules plus Connected Glass, retained for visual research.
    registry.emplace<AimAssistModule>();
    registry.emplace<AutoHitModule>();
    registry.emplace<MaceSwapperModule>();
    registry.emplace<UnglideModule>();
    registry.emplace<SwordSwapperModule>();
    registry.emplace<HitboxExpanderModule>();
    registry.emplace<UltrakillHudModule>();
    registry.emplace<ModuleDiagnosticsModule>();
    registry.emplace<ConnectedGlassModule>();
    registry.emplace<EspModule>();
}
