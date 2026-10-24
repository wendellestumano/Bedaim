#pragma once

#include <nlohmann/json.hpp>
#include "../core/Diagnostics.hpp"
#include "../core/KeybindEdge.hpp"
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
        if (key != "keybind") return;
        // Key-down repeats must not repeatedly toggle the module. Key-up only
        // releases the edge latch; it never changes the active state.
        if (m_toggleKeyEdge.update(isDown)) {
            setKeybindActive(!keybindActive);
        }
    }

    void setMasterEnabled(bool state) {
        if (masterEnabled == state) return;
        masterEnabled = state;
        // A module disabled from the menu may stop receiving key-up events.
        // Clear the latch on a master-state transition so the next press works.
        m_toggleKeyEdge.reset();
        updateEnabledState();
    }

    void setKeybindActive(bool state) {
        if (keybindActive == state) return;
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

private:
    bedrocktools::core::KeybindEdge m_toggleKeyEdge;
};