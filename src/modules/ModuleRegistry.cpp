#include "ModuleRegistry.hpp"
#include "../core/Diagnostics.hpp"

#include "player/aimassist.hpp"
#include "player/autohit.hpp"
#include "player/maceswapper.hpp"
#include "player/unglide.hpp"
#include "player/swordswapper.hpp"
#include "visual/esp.hpp"
#include "visual/blockesp.hpp"
#include "visual/hitbox.hpp"
#include "visual/oreesp.hpp"
#include "hud/ultrakillhud.hpp"
#include "misc/modulediagnostics.hpp"
#include "misc/configurations.hpp"
#include "misc/seedmap.hpp"
#include "misc/seedmap.hpp"

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
        "ModuleRegistry", std::string("registered Bedaim modules initialized: ") + std::to_string(mView.size()));
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

    registry.emplace<AimAssistModule>();
    registry.emplace<AutoHitModule>();
    registry.emplace<MaceSwapperModule>();
    registry.emplace<UnglideModule>();
    registry.emplace<SwordSwapperModule>();
    registry.emplace<HitboxExpanderModule>();
    registry.emplace<HitboxModule>(); // Shared RenderLevel renderer used by ESP and Ore ESP.
    registry.emplace<EspModule>();
    registry.emplace<BlockEspModule>();
    registry.emplace<OreEspModule>();
    registry.emplace<UltrakillHudModule>();
    registry.emplace<ModuleDiagnosticsModule>();
    registry.emplace<ConfigurationsModule>();
    registry.emplace<SeedMapModule>();
    registry.emplace<SeedMapModule>();
}
