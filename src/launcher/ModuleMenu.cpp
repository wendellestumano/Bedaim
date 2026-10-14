#include "ModuleMenu.hpp"
#include "modules/ModuleRegistry.hpp"
#include "config/ConfigManager.hpp"
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

static void onModuleToggle(std::string_view module_id, bool enabled) {
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    mod->setMasterEnabled(enabled);
    bedrocktools::config::ConfigManager::get().save();
}

static void onModuleKeybind(std::string_view module_id, std::string_view key, bool isDown) {
    if (ModuleRegistry::get().keybindBlocked()) return;
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    mod->onKeybindEvent(std::string(key), isDown);
}

static void onModuleConfigChanged(std::string_view module_id, std::string_view key, std::string_view value) {
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    if (mod->onMenuConfigChanged(key, value)) {
        bedrocktools::config::ConfigManager::get().save();
        return;
    }

    nlohmann::json j;
    mod->saveConfig(j);

    std::string safeValue(value);
    std::string safeKey(key);
    try {
        if (j.contains(safeKey)) {
            if (j[safeKey].is_boolean()) {
                if (safeValue == "true") j[safeKey] = true;
                else if (safeValue == "false") j[safeKey] = false;
            } else if (j[safeKey].is_number_integer()) {
                if (!safeValue.empty()) {
                    char* end;
                    int val = std::strtol(safeValue.c_str(), &end, 10);
                    if (end != safeValue.c_str()) j[safeKey] = val;
                }
            } else if (j[safeKey].is_number_float()) {
                if (!safeValue.empty()) {
                    char* end;
                    float val = std::strtof(safeValue.c_str(), &end);
                    if (end != safeValue.c_str()) j[safeKey] = val;
                }
            } else {
                j[safeKey] = safeValue;
            }
        } else {
            j[safeKey] = safeValue;
        }
    } catch (...) {
        j[safeKey] = safeValue;
    }
    mod->loadConfig(j);
    bedrocktools::config::ConfigManager::get().save();
}

void registerModulesWithLauncher() {
    auto& modules = ModuleRegistry::get().modules();

    for (auto* mod : modules) {
        if (!mod->showInMenu) continue;

        pl::modmenu::ModuleBuilder builder(mod->moduleId, mod->name);
        builder.description(mod->description)
                .defaultEnabled(mod->masterEnabled)
                .hideInHudEditor(mod->hideInHudEditor)
                .onToggle(onModuleToggle)
                .onConfigChanged(onModuleConfigChanged)
                .onKeybind(onModuleKeybind);

        nlohmann::json j;
        mod->saveConfig(j);

        static const char* baseKeys[] = {
                "enabled", "masterEnabled", "keybindActive", "shortcutEnabled", "shortcutSize", "shortcutOpacity",
                "lockPosition", "shortcutPosX", "shortcutPosY",
                "isHudModule", "hudPosX", "hudPosY"
        };

        struct TmpConfigEntry {
            std::string key;
            std::string displayName;
            pl::modmenu::ConfigType type;
            std::string default_value;
            std::string min_value;
            std::string max_value;
            std::string depends_on;
        };
        std::vector<TmpConfigEntry> configs;

        for (auto& [k, v] : j.items()) {
            bool isBase = false;
            for (const char* bk : baseKeys) {
                if (k == bk) { isBase = true; break; }
            }
            std::string keyLower = k;
            std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);
            const bool isHudPosition = keyLower.rfind("hud", 0) == 0 &&
                (keyLower.ends_with("posx") || keyLower.ends_with("posy"));
            if (isBase || isHudPosition || !mod->showInLegacyMenu(k)) continue;

            std::string displayName;
            std::string sourceKey = k;
            if (sourceKey.size() > 2 && sourceKey[0] == 'm' && sourceKey[1] == '_') {
                sourceKey = sourceKey.substr(2);
            }
            for (size_t i = 0; i < sourceKey.size(); ++i) {
                if (i == 0) {
                    displayName += toupper(sourceKey[i]);
                } else if (isupper(sourceKey[i])) {
                    displayName += ' ';
                    displayName += sourceKey[i];
                } else {
                    displayName += sourceKey[i];
                }
            }

            if (mod->moduleId == "bedrocktools.Mace Swapper") {
                if (k == "m_attackRange") displayName = "MACE SWAPPER > Range";
                else if (k == "m_attackIntervalMs") displayName = "MACE SWAPPER > Attack interval (ms)";
                else if (k == "m_restoreSlot") displayName = "MACE SWAPPER > Restore previous slot";
            }

            if (mod->moduleId == "bedrocktools.Aim Assist") {
                if (k == "m_aimMode") displayName = "AIM OPTIONS > Profile";
                else if (k == "m_trackingMode") displayName = "AIM OPTIONS > Tracking mode";
                else if (k == "m_naturalization") displayName = "AIM OPTIONS > Naturalization during jumps";
                else if (k == "m_naturalizationWeapon") displayName = "AIM OPTIONS > Naturalization weapon";
                else if (k == "m_fov") displayName = "BEHAVIOR > FOV";
                else if (k == "m_targetSwitchHysteresis") displayName = "TARGETING > Crosshair switch stability";
                else if (k == "m_targetSwitchDistanceMargin") displayName = "TARGETING > Distance switch margin";
                else if (k == "m_targetSelectionMode") displayName = "TARGETING > Target selection mode";
                else if (k == "m_targetRetentionRadius") displayName = "TARGETING > Around-player lock radius (blocks)";
                else if (k == "m_autoHit") displayName = "AUTO HIT > Enable";
                else if (k == "m_autoHitRange") displayName = "AUTO HIT > Range (blocks)";
                else if (k == "m_autoHitDelayMs") displayName = "AUTO HIT > Attack interval (ms)";
                else if (k == "m_range") displayName = "BEHAVIOR > Range";
                else if (k == "m_aimSpeed") displayName = "BEHAVIOR > Aim speed";
                else if (k == "m_strength") displayName = "BEHAVIOR > Strongness";
                else if (k == "m_hitboxScale") displayName = "BEHAVIOR > Hitbox area";
                else if (k == "m_targetPlayers") displayName = "TARGETING > Players";
                else if (k == "m_targetMobs") displayName = "TARGETING > Mobs";
                else if (k == "m_cantSeeThroughBlocks") displayName = "TARGETING > Require line of sight";
                else if (k == "m_whitelistNames") displayName = "TARGETING > Ignore names";
                else if (k == "m_weaponOnly") displayName = "WEAPON FILTER > Enable filter";
                else if (k == "m_weaponSword") displayName = "WEAPON FILTER > Sword";
                else if (k == "m_weaponAxe") displayName = "WEAPON FILTER > Axe";
                else if (k == "m_weaponMace") displayName = "WEAPON FILTER > Mace";
                else if (k == "m_weaponBow") displayName = "WEAPON FILTER > Bow";
                else if (k == "m_weaponCrossbow") displayName = "WEAPON FILTER > Crossbow";
                else if (k == "m_weaponTrident") displayName = "WEAPON FILTER > Trident";
                else if (k == "m_weaponSpear") displayName = "WEAPON FILTER > Spear";
                else if (k == "m_weaponOtherMelee") displayName = "WEAPON FILTER > Other melee";
                else if (k == "m_elytraMode") displayName = "SPECIAL > Elytra mode";
            }

            TmpConfigEntry entry;
            entry.key = k;
            entry.displayName = displayName;

            if (v.is_boolean()) {
                std::string kLower = k;
                std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);
                if (kLower.find("button") != std::string::npos) {
                    entry.type = pl::modmenu::ConfigType::Button;
                } else {
                    entry.type = pl::modmenu::ConfigType::Toggle;
                }
                entry.default_value = v.get<bool>() ? "true" : "false";
            } else if (v.is_number_integer()) {
                entry.type = pl::modmenu::ConfigType::SliderInt;
                entry.default_value = std::to_string(v.get<int>());

                std::string kLower = k;
                std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);

                if (kLower.find("keybind") != std::string::npos) {
                    entry.type = pl::modmenu::ConfigType::Keybind;
                } else {
                    int minVal = 0;
                    int maxVal = 200;
                    if (kLower.find("cps") != std::string::npos) {
                        minVal = 1;
                        maxVal = 30;
                    } else if (kLower.find("time") != std::string::npos) {
                        maxVal = 24000;
                    }

                    entry.min_value = std::to_string(minVal);
                    entry.max_value = std::to_string(maxVal);
                }
            } else if (v.is_number_float()) {
                entry.type = pl::modmenu::ConfigType::SliderFloat;
                entry.default_value = std::to_string(v.get<float>());

                std::string kLower = k;
                std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);

                float minVal = 0.0f;
                float maxVal = 100.0f;
                if (mod->moduleId == "bedrocktools.Mace Swapper" && kLower == "m_attackrange") {
                    minVal = 2.0f;
                    maxVal = 6.0f;
                } else if (mod->moduleId == "bedrocktools.Mace Swapper" && kLower == "m_attackintervalms") {
                    minVal = 80.0f;
                    maxVal = 1000.0f;
                } else if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_strength") {
                    minVal = 0.0f;
                    maxVal = 1.0f;
                } else if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_aimspeed") {
                    minVal = 0.1f;
                    maxVal = 10.0f;
                } else if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_hitboxscale") {
                    minVal = 0.5f;
                    maxVal = 1.0f;
                } else if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_targetswitchhysteresis") {
                    minVal = 0.0f;
                    maxVal = 12.0f;
                } else if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_targetswitchdistancemargin") {
                    minVal = 0.0f;
                    maxVal = 5.0f;
                } else if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_targetretentionradius") {
                    minVal = 0.5f;
                    maxVal = 5.0f;
                } else if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_autohitrange") {
                    minVal = 2.0f;
                    maxVal = 6.0f;
                } else if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_autohitdelayms") {
                    minVal = 80.0f;
                    maxVal = 1000.0f;
                } else if (kLower.find("opacity") != std::string::npos ||
                    kLower.find("color") != std::string::npos ||
                    kLower.find("alpha") != std::string::npos) {
                    maxVal = 1.0f;
                } else if (kLower.find("scale") != std::string::npos) {
                    minVal = 0.1f;
                    maxVal = 5.0f;
                } else if (kLower == "borderwidth") {
                    maxVal = 4.0f;
                } else if (kLower.find("width") != std::string::npos) {
                    maxVal = 1000.0f;
                } else if (kLower.find("position") != std::string::npos ||
                           kLower.find("posx") != std::string::npos ||
                           kLower.find("posy") != std::string::npos) {
                    maxVal = 2000.0f;
                } else if (kLower.find("range") != std::string::npos) {
                    maxVal = 180.0f;
                    if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_range") {
                        minVal = 2.0f;
                        maxVal = 80.0f;
                    }
                } else if (kLower.find("fov") != std::string::npos) {
                    minVal = 1.0f;
                    maxVal = 179.0f;
                    if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_fov") {
                        minVal = 1.0f;
                        maxVal = 360.0f;
                    }
                } else if (kLower.find("intensity") != std::string::npos) {
                    maxVal = 10.0f;
                } else if (kLower.find("speed") != std::string::npos || kLower.find("strength") != std::string::npos) {
                    minVal = 0.05f;
                    maxVal = 1.0f;
                } else if (kLower.find("thick") != std::string::npos) {
                    maxVal = 20.0f;
                }

                entry.min_value = std::to_string(minVal);
                entry.max_value = std::to_string(maxVal);
            } else if (v.is_string()) {
                std::string str = v.get<std::string>();
                if (mod->moduleId == "bedrocktools.Aim Assist" && k == "m_trackingMode") {
                    entry.type = pl::modmenu::ConfigType::Radio;
                    if (str == "Grudado") entry.default_value = "0";
                    else if (str == "Adaptativo") entry.default_value = "2";
                    else entry.default_value = "1";
                    entry.min_value = "Grudado,Smooth,Adaptativo";

                } else if (mod->moduleId == "bedrocktools.Aim Assist" && k == "m_targetSelectionMode") {
                    entry.type = pl::modmenu::ConfigType::Radio;
                    entry.default_value = (str == "Player") ? "0" : "1";
                    entry.min_value = "Closest to player,Closest to crosshair";

                } else if (mod->moduleId == "bedrocktools.Aim Assist" && k == "m_naturalizationWeapon") {
                    entry.type = pl::modmenu::ConfigType::Radio;
                    const std::vector<std::string> weapons = {
                        "All", "Sword", "Axe", "Mace", "Bow", "Crossbow", "Trident", "Spear", "Other melee"
                    };
                    const auto selected = std::find(weapons.begin(), weapons.end(), str);
                    entry.default_value = selected == weapons.end()
                        ? "0" : std::to_string(std::distance(weapons.begin(), selected));
                    entry.min_value = "All,Sword,Axe,Mace,Bow,Crossbow,Trident,Spear,Other melee";

                } else if (mod->moduleId == "bedrocktools.Aim Assist" && k == "m_aimMode") {
                    entry.type = pl::modmenu::ConfigType::Radio;
                    if (str == "Robotic") entry.default_value = "0";
                    else if (str == "Human") entry.default_value = "2";
                    else entry.default_value = "1";
                    entry.min_value = "Robotic,Classic,Human";

                } else if (str.find(',') != std::string::npos) {
                    entry.type = pl::modmenu::ConfigType::Radio;
                    size_t firstComma = str.find(',');
                    entry.default_value = str.substr(0, firstComma);
                    entry.min_value = str.substr(firstComma + 1);
                } else if (!str.empty() && str[0] == '#') {
                    entry.type = pl::modmenu::ConfigType::Color;
                    entry.default_value = str;
                } else {
                    entry.type = pl::modmenu::ConfigType::Text;
                    entry.default_value = str;
                }
            } else {
                continue;
            }
            configs.push_back(entry);
        }

        if (mod->moduleId == "bedrocktools.Aim Assist") {
            // Keep the flat launcher menu visually grouped by consistent prefixes
            // and a deliberate order; the current public menu API has no section header.
            const std::vector<std::string> priority = {
                "m_aimMode", "m_trackingMode", "m_naturalization", "m_naturalizationWeapon",
                "m_fov", "m_range", "m_aimSpeed", "m_strength", "m_hitboxScale",
                "m_targetSwitchHysteresis", "m_targetSwitchDistanceMargin", "m_targetSelectionMode",
                "m_targetRetentionRadius", "m_autoHit", "m_autoHitRange", "m_autoHitDelayMs", "m_targetPlayers", "m_targetMobs", "m_cantSeeThroughBlocks", "m_whitelistNames",
                "m_weaponOnly", "m_weaponSword", "m_weaponAxe", "m_weaponMace",
                "m_weaponBow", "m_weaponCrossbow", "m_weaponTrident", "m_weaponSpear",
                "m_weaponOtherMelee", "m_elytraMode"
            };
            const auto rank = [&priority](const std::string& key) {
                const auto it = std::find(priority.begin(), priority.end(), key);
                return static_cast<std::size_t>(std::distance(priority.begin(), it));
            };
            std::stable_sort(configs.begin(), configs.end(),
                [&rank](const TmpConfigEntry& a, const TmpConfigEntry& b) {
                    return rank(a.key) < rank(b.key);
                });
        }

        for (auto& entry : configs) {
            std::string k = entry.key;
            if (mod->moduleId == "bedrocktools.Aim Assist" &&
                (k == "m_weaponSword" || k == "m_weaponAxe" || k == "m_weaponMace" ||
                 k == "m_weaponBow" || k == "m_weaponCrossbow" || k == "m_weaponTrident" ||
                 k == "m_weaponSpear" || k == "m_weaponOtherMelee")) {
                entry.depends_on = "m_weaponOnly";
            }
            if (entry.type != pl::modmenu::ConfigType::Toggle) {
                std::string bestParent = "";
                for (const auto& parentCandidate : configs) {
                    if (parentCandidate.type == pl::modmenu::ConfigType::Toggle) {
                        std::string pKey = parentCandidate.key;
                        if (k.length() > pKey.length() && k.compare(0, pKey.length(), pKey) == 0) {
                            if (pKey.length() > bestParent.length()) {
                                bestParent = pKey;
                            }
                        }
                    }
                }
                if (!bestParent.empty() && entry.depends_on.empty()) entry.depends_on = bestParent;
            }
            builder.config(entry.key, entry.displayName, entry.type, entry.default_value, entry.min_value, entry.max_value, entry.depends_on);
        }

        if (builder.registerModule()) mod->onMenuRegistered();
    }
}
