#pragma once

#include <nlohmann/json.hpp>
#include "../core/Diagnostics.hpp"
#include <string>
#include <string_view>
#include <utility>

class Module {
public:
    const char* name;
    const char* description;
    std::string moduleId;
    bool masterEnabled = false;
    bool keybindActive = true;
    bool enabled = false;
    bool showInMenu = true;
    bool hideInHudEditor = false;
    int keybind = 0;

    // Keep the stable internal ID independent from the user-facing module name.
    // This lets modules be branded without breaking other modules or saved configs.
    Module(const char* n, const char* d, const char* idName = nullptr)
        : name(n), description(d),
          moduleId(std::string("bedrocktools.") + (idName ? idName : n)) {}
    virtual ~Module() = default;

    virtual void onInit()     {}
    virtual void onEnable()   {}
    virtual void onDisable()  {}
    virtual void onFrame()    {}
    virtual bool onMouseEvent(int button, bool isDown) { return false; }
    virtual bool onMenuConfigChanged(std::string_view key, std::string_view value) { return false; }
    virtual bool showInLegacyMenu(std::string_view key) const { return true; }
    virtual void onMenuRegistered() {}
    
    virtual void onKeybindEvent(const std::string& key, bool isDown) {
        if (key == "keybind" && isDown) {
            setKeybindActive(!keybindActive);
        }
    }

    void setMasterEnabled(bool state) {
        // Never short-circuit on the stored master flag. A stale effective enabled
        // flag can survive an earlier transition; always recompute it from both gates.
        masterEnabled = state;
        updateEnabledState();
    }

    void setKeybindActive(bool state) {
        // Reconcile even when the value is repeated, for the same reason as the
        // master toggle: effective state is the conjunction of both gates.
        keybindActive = state;
        updateEnabledState();
    }

    void updateEnabledState() {
        bool newState = masterEnabled && keybindActive;
        if (newState != enabled) {
            enabled = newState;
            bedrocktools::diagnostics::log(
                bedrocktools::diagnostics::Level::Info,
                moduleId, enabled ? "module enabled" : "module disabled");
            if (enabled) onEnable();
            else onDisable();
        }
    }

    virtual void loadConfig(const nlohmann::json& j) {
        if (j.contains("keybind")) keybind = j["keybind"].get<int>();
        if (j.contains("keybindActive")) {
            keybindActive = j["keybindActive"].get<bool>();
            updateEnabledState();
        }
        if (j.contains("masterEnabled")) {
            masterEnabled = j["masterEnabled"].get<bool>();
            updateEnabledState();
        }
    }

    virtual void saveConfig(nlohmann::json& j) {
        j["keybind"] = keybind;
        j["keybindActive"] = keybindActive;
        j["masterEnabled"] = masterEnabled;
    }
};
