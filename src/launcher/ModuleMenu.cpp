#include "ModuleMenu.hpp"
#include "MenuConfigValue.hpp"
#include "modules/ModuleRegistry.hpp"
#include "config/ConfigManager.hpp"
#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

static void registerAimAssistConfigSchema(std::string_view moduleId, const nlohmann::json& config);
static void registerHitboxExpanderConfigSchema(std::string_view moduleId, const nlohmann::json& config);
static void registerUltrakillHudConfigSchema(std::string_view moduleId, const nlohmann::json& config);
static void scheduleModuleConfigSchemaRefresh(Module* mod, std::string_view key, const nlohmann::json& snapshot);

static void persistModuleConfig() {
    // Menu callbacks can fire on every slider movement or every typed character.
    // ConfigManager already debounces and writes on its worker thread; never force
    // a synchronous disk flush on the UI/input thread.
    bedrocktools::config::ConfigManager::get().save();
}

static void onModuleToggle(std::string_view module_id, bool enabled) {
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    mod->setMasterEnabled(enabled);
    persistModuleConfig();
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
        // Snapshot the post-callback settings. V2 schema changes are debounced below
        // so live sliders and text inputs are not rebuilt on every edit event.
        nlohmann::json snapshot;
        mod->saveConfig(snapshot);
        scheduleModuleConfigSchemaRefresh(mod, key, snapshot);
        persistModuleConfig();
        return;
    }

    nlohmann::json j;
    mod->saveConfig(j);

    try {
        const auto result = bedaim::menuconfig::applyValue(j, key, value);
        if (result == bedaim::menuconfig::ApplyResult::InvalidValue) return;
    } catch (...) {
        return;
    }

    // loadConfig normalizes radio indices and clamps sliders. Retain that canonical
    // snapshot and refresh the V2 schema after input activity settles.
    mod->loadConfig(j);
    nlohmann::json snapshot;
    mod->saveConfig(snapshot);
    scheduleModuleConfigSchemaRefresh(mod, key, snapshot);
    persistModuleConfig();
}

static std::string schemaCurrentValue(const nlohmann::json& config,
                                     const std::string& key,
                                     const std::string& fallback) {
    const auto it = config.find(key);
    if (it == config.end() || it->is_null()) return fallback;
    if (it->is_string()) return it->get<std::string>();
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    if (it->is_number()) return it->dump();
    return fallback;
}

static void registerAimAssistConfigSchema(std::string_view moduleId,
                                          const nlohmann::json& config) {
    using namespace pl::modmenu;
    static nlohmann::json initialDefaults;
    static bool defaultsCaptured = false;
    if (!defaultsCaptured) {
        initialDefaults = config;
        defaultsCaptured = true;
    }
    ConfigSchemaBuilder schema;
    schema.defaultCategory("aim")
        .category("aim", "Aim", "Rotation, behavior and tracking profile")
        .category("targeting", "Targeting", "Target selection and visibility")
        .category("hitbox", "Hitbox", "Virtual hitbox area used for steering")
        .category("auto_hit", "Auto Hit", "Attack distance and cooldown")
        .category("weapons", "Weapons", "Item filters")
        .category("special", "Special", "Elytra and jump behavior");

    auto addNode = [&](const std::string& key, const std::string& title,
                       const std::string& category, ConfigControlTypeV2 type,
                       const std::string& minValue, const std::string& maxValue,
                       const std::string& step, const std::string& unit,
                       const std::string& description, bool advanced,
                       const std::vector<std::pair<std::string, std::string>>& choices,
                       ConfigChoiceStyleV2 choiceStyle) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.category = category;
        node.title = title;
        node.description = description;
        node.type = type;
        node.choiceStyle = choiceStyle;
        node.defaultValue = schemaCurrentValue(initialDefaults, key, schemaCurrentValue(config, key, ""));
        node.currentValue = schemaCurrentValue(config, key, "");
        node.hasCurrentValue = config.contains(key);
        node.minValue = minValue;
        node.maxValue = maxValue;
        node.step = step;
        node.unit = unit;
        node.advanced = advanced;
        for (const auto& choice : choices) {
            ConfigOptionV2 option;
            option.value = choice.first;
            option.label = choice.second;
            node.options.push_back(std::move(option));
        }
        schema.node(std::move(node));
    };

    auto toggle = [&](const char* key, const char* title, const char* category,
                      const char* description, bool advanced = false) {
        addNode(key, title, category, ConfigControlTypeV2::Toggle, "", "", "", "",
                description, advanced, {}, ConfigChoiceStyleV2::Auto);
    };
    auto slider = [&](const char* key, const char* title, const char* category,
                      const char* minValue, const char* maxValue, const char* step,
                      const char* unit, const char* description, bool advanced = false) {
        addNode(key, title, category, ConfigControlTypeV2::SliderFloat, minValue, maxValue,
                step, unit, description, advanced, {}, ConfigChoiceStyleV2::Auto);
    };
    auto choice = [&](const char* key, const char* title, const char* category,
                      const char* description,
                      const std::vector<std::pair<std::string, std::string>>& options,
                      ConfigChoiceStyleV2 style, bool advanced = false) {
        addNode(key, title, category, ConfigControlTypeV2::Choice, "", "", "", "",
                description, advanced, options, style);
    };

    choice("m_aimMode", "Aim profile", "aim", "Select the overall aim response.",
           {{"Robotic","Robotic"},{"Classic","Classic"},{"Human","Human"}},
           ConfigChoiceStyleV2::Segmented);
    choice("m_trackingMode", "Tracking", "aim", "How the assist follows the selected hitbox.",
           {{"Grudado","Sticky"},{"Smooth","Smooth"},{"Adaptativo","Adaptive"}},
           ConfigChoiceStyleV2::Segmented);
    slider("m_aimSpeed", "Aim speed", "aim", "0.1", "10", "0.1", "x",
           "Base angular turn rate.");
    slider("m_rotationSpeed", "Rotation speed", "aim", "0.1", "1000", "0.1", "x",
           "Independent multiplier for large rotations and targets behind you; values up to 1000 apply immediately.");
    choice("m_predictionMode", "Target prediction", "aim",
           "Predict motion for ranged weapons, or for all weapons.",
           {{"Off","Off"},{"Ranged","Ranged weapons"},{"All","All weapons"}},
           ConfigChoiceStyleV2::Segmented);
    slider("m_predictionLeadMs", "Prediction lead", "aim", "0", "300", "10", "ms",
           "How far ahead to estimate a moving target.");
    slider("m_strength", "Strongness", "aim", "0", "1", "0.05", "",
           "How much the assist resists opposing manual drag.");
    slider("m_fov", "FOV", "aim", "1", "360", "1", "deg",
           "Maximum angular search region.");
    slider("m_range", "Detection range", "aim", "2", "80", "0.5", "blocks",
           "How far the target scanner searches.");

    toggle("m_targetPlayers", "Target players", "targeting", "Include player entities.");
    toggle("m_targetMobs", "Target mobs", "targeting", "Include supported mob entities.");
    toggle("m_ignoreCreativeSpectator", "Ignore Creative/Spectator players", "targeting",
           "Skip players in Creative or Spectator when the runtime exposes the native game-type method.");
    toggle("m_cantSeeThroughBlocks", "Require line of sight", "targeting",
           "Reject targets obstructed by solid blocks.");
    choice("m_targetSelectionMode", "Target priority", "targeting",
           "Choose the nearest entity or the entity closest to the crosshair.",
           {{"Player","Closest to player"},{"Crosshair","Closest to crosshair"}},
           ConfigChoiceStyleV2::Segmented);
    slider("m_targetSwitchHysteresis", "Crosshair switch stability", "targeting",
           "0", "12", "0.25", "deg", "Reduce target changes while tracking.", true);
    slider("m_targetSwitchDistanceMargin", "Distance switch margin", "targeting",
           "0", "5", "0.1", "blocks", "Keep the current target unless a challenger is clearly closer.", true);
    slider("m_targetRetentionRadius", "Around-player lock radius", "targeting",
           "0.5", "5", "0.1", "blocks", "Keep a nearby target during fast camera movement.");
    addNode("m_whitelistNames", "Ignore names", "targeting", ConfigControlTypeV2::Text,
            "", "", "", "", "Comma-separated names to exclude.", true, {},
            ConfigChoiceStyleV2::Auto);

    slider("m_hitboxScale", "Base hitbox area", "hitbox", "0.30", "50", "0.10", "x",
           "Base size of the virtual steering region.");
    toggle("m_distanceDependentHitbox", "Distance-dependent hitbox", "hitbox",
           "Tighten near targets and expand the virtual aim region with distance.");
    slider("m_hitboxDistanceGrowth", "Distance hitbox strength", "hitbox", "0.1", "500",
           "0.1", "x", "Higher values expand the virtual steering volume more at long range; this does not alter server collision.");
    toggle("m_fourPointAiming", "Four-point hitbox aiming", "hitbox",
           "Select the closest of four stable hitbox anchors, then keep the existing slide behavior.");

    toggle("m_autoHit", "Enable Auto Hit", "auto_hit",
           "Attempt an attack on a valid target within range.");
    toggle("m_autoHitButtonEnabled", "Show AutoHit floating button", "auto_hit",
           "Show or hide the launcher Hold button without disabling AutoHit itself.");
    slider("m_autoHitRange", "Attack range", "auto_hit", "2", "6", "0.1", "blocks",
           "Maximum AutoHit point distance. Its actor scan extends to at least this range.");
    slider("m_autoHitDelayMs", "Attack interval", "auto_hit", "80", "1000", "10", "ms",
           "Minimum time between native attack attempts.");
    addNode("m_autoHitKeyBinding", "Attack input key", "auto_hit", ConfigControlTypeV2::Text,
            "", "", "", "",
            "Type button1 for physical mouse button 1, shift, 0, a, or keycode:188. Mouse bindings observe input without consuming clicks. Keyboard aliases expose a floating Hold button. Use auto for always-on native AutoHit.",
            true, {}, ConfigChoiceStyleV2::Auto);

    toggle("m_weaponOnly", "Enable weapon filter", "weapons",
           "Run Aim Assist only while a selected weapon type is held.");
    toggle("m_weaponSword", "Sword", "weapons", "Allow swords when the filter is enabled.");
    toggle("m_weaponAxe", "Axe", "weapons", "Allow axes when the filter is enabled.");
    toggle("m_weaponMace", "Mace", "weapons", "Allow the mace when the filter is enabled.");
    toggle("m_weaponBow", "Bow", "weapons", "Allow bows when the filter is enabled.");
    toggle("m_weaponCrossbow", "Crossbow", "weapons", "Allow crossbows when the filter is enabled.");
    toggle("m_weaponTrident", "Trident", "weapons", "Allow tridents when the filter is enabled.");
    toggle("m_weaponSpear", "Spear", "weapons", "Allow spears when the filter is enabled.");
    toggle("m_weaponOtherMelee", "Other melee", "weapons", "Allow other melee weapons.");
    slider("m_swordAimMultiplier", "Sword aim profile", "weapons", "0.2", "10", "0.1", "x",
           "Aim-speed multiplier while a sword is held.");
    slider("m_axeAimMultiplier", "Axe aim profile", "weapons", "0.2", "10", "0.1", "x",
           "Aim-speed multiplier while an axe is held.");
    slider("m_maceAimMultiplier", "Mace aim profile", "weapons", "0.2", "10", "0.1", "x",
           "Aim-speed multiplier while the mace is held.");
    slider("m_swordAimPointY", "Sword hitbox Y point", "weapons", "0.05", "0.95", "0.05", "Y",
           "Aim height as a fraction of target height: 0.05 is near feet, 0.95 is near head.");
    slider("m_axeAimPointY", "Axe hitbox Y point", "weapons", "0.05", "0.95", "0.05", "Y",
           "Aim height as a fraction of target height.");
    slider("m_maceAimPointY", "Mace hitbox Y point", "weapons", "0.05", "0.95", "0.05", "Y",
           "Higher values aim closer to the upper hitbox; default is 0.80.");
    slider("m_bowAimPointY", "Bow hitbox Y point", "weapons", "0.05", "0.95", "0.05", "Y",
           "Aim height as a fraction of target height.");
    slider("m_crossbowAimPointY", "Crossbow hitbox Y point", "weapons", "0.05", "0.95", "0.05", "Y",
           "Aim height as a fraction of target height.");
    slider("m_tridentAimPointY", "Trident hitbox Y point", "weapons", "0.05", "0.95", "0.05", "Y",
           "Aim height as a fraction of target height.");
    slider("m_spearAimPointY", "Spear hitbox Y point", "weapons", "0.05", "0.95", "0.05", "Y",
           "Aim height as a fraction of target height.");
    slider("m_otherMeleeAimPointY", "Other melee hitbox Y point", "weapons", "0.05", "0.95", "0.05", "Y",
           "Aim height as a fraction of target height.");
    slider("m_bowAimMultiplier", "Bow aim profile", "weapons", "0.2", "10", "0.1", "x",
           "Aim-speed multiplier while a bow is held.");
    slider("m_crossbowAimMultiplier", "Crossbow aim profile", "weapons", "0.2", "10", "0.1", "x",
           "Aim-speed multiplier while a crossbow is held.");
    slider("m_tridentAimMultiplier", "Trident aim profile", "weapons", "0.2", "10", "0.1", "x",
           "Aim-speed multiplier while a trident is held.");
    slider("m_spearAimMultiplier", "Spear aim profile", "weapons", "0.2", "10", "0.1", "x",
           "Aim-speed multiplier while a spear is held.");
    slider("m_otherMeleeAimMultiplier", "Other melee aim profile", "weapons", "0.2", "10", "0.1", "x",
           "Aim-speed multiplier while another melee weapon is held.");

    toggle("m_naturalization", "Naturalize jumps", "special",
           "Temporarily reduce pitch correction during a jump.");
    choice("m_naturalizationWeapon", "Naturalization weapon", "special",
           "Choose when jump naturalization applies.",
           {{"All","All"},{"Sword","Sword"},{"Axe","Axe"},{"Mace","Mace"},
            {"Bow","Bow"},{"Crossbow","Crossbow"},{"Trident","Trident"},
            {"Spear","Spear"},{"Other melee","Other melee"},{"Tools","Tools"}},
           ConfigChoiceStyleV2::Dropdown);
    toggle("m_elytraMode", "Elytra mode", "special",
           "Enable small aim-point corrections while gliding.");

    if (!setConfigSchemaJson(moduleId, schema.toJson())) {
        // Legacy controls remain registered as a fallback when a loader version
        // does not accept the V2 schema.
    }
}

static void registerHitboxExpanderConfigSchema(std::string_view moduleId,
                                               const nlohmann::json& config) {
    using namespace pl::modmenu;
    static nlohmann::json initialDefaults;
    static bool defaultsCaptured = false;
    if (!defaultsCaptured) {
        initialDefaults = config;
        defaultsCaptured = true;
    }
    ConfigSchemaBuilder schema;
    schema.defaultCategory("hitbox")
        .category("hitbox", "Hitbox", "Client-side target geometry; the server still validates attacks.");
    auto add = [&](const char* key, const char* title, ConfigControlTypeV2 type,
                   const char* minValue, const char* maxValue, const char* step,
                   const char* unit, const char* description) {
        const std::string k(key);
        ConfigNodeV2 node;
        node.id = k;
        node.key = k;
        node.category = "hitbox";
        node.title = title;
        node.description = description;
        node.type = type;
        node.defaultValue = schemaCurrentValue(initialDefaults, k, schemaCurrentValue(config, k, ""));
        node.currentValue = schemaCurrentValue(config, k, "");
        node.hasCurrentValue = config.contains(k);
        node.minValue = minValue;
        node.maxValue = maxValue;
        node.step = step;
        node.unit = unit;
        schema.node(std::move(node));
    };
    add("m_hitboxWidth", "Width / depth", ConfigControlTypeV2::SliderFloat,
        "0.6", "8", "0.1", "blocks", "Minimum X/Z width of the client-side target box.");
    add("m_hitboxHeight", "Height", ConfigControlTypeV2::SliderFloat,
        "0.6", "8", "0.1", "blocks", "Minimum height; larger native boxes are preserved.");
    add("m_expandPlayers", "Expand players", ConfigControlTypeV2::Toggle,
        "", "", "", "", "Apply expanded target geometry to players.");
    add("m_expandMobs", "Expand mobs", ConfigControlTypeV2::Toggle,
        "", "", "", "", "Apply expanded target geometry to supported mobs.");
    add("m_requireLineOfSight", "Require line of sight", ConfigControlTypeV2::Toggle,
        "", "", "", "", "Require a clear block ray for expanded target checks.");
    if (!setConfigSchemaJson(moduleId, schema.toJson())) {
        // Legacy menu controls remain available on loaders without V2 schema support.
    }
}

static void registerUltrakillHudConfigSchema(std::string_view moduleId,
                                               const nlohmann::json& config) {
    using namespace pl::modmenu;
    static nlohmann::json initialDefaults;
    static bool defaultsCaptured = false;
    if (!defaultsCaptured) {
        initialDefaults = config;
        defaultsCaptured = true;
    }

    ConfigSchemaBuilder schema;
    schema.defaultCategory("hud")
        .category("hud", "ULTRAKILL HUD", "Visual overlay and impact feedback");

    for (const auto& [key, title, description] : {
             std::tuple<const char*, const char*, const char*>{
                 "m_showMaceStatus", "Weapon / movement panel", "Show the selected item and movement readouts."},
             {"m_showStyleCounter", "Style score and combo", "Track confirmed hits, score, combo, and style rank."},
             {"m_showReticle", "Crosshair reticle", "Draw the ULTRAKILL-style center reticle."},
             {"m_enableParryFlash", "Attack / parry flash", "Show a visual pulse when an attack is attempted."},
             {"m_enableImpactShake", "Impact shake", "Add a brief visual screen-shake effect after an attack."},
             {"m_enableHitStop", "Extended impact hold (visual only)", "Extend the HUD impact effect without cancelling BedrockTools frame events."},
             {"m_showImpactText", "Impact text", "Show HIT CONFIRMED / MACE HIT / GROUND SLAM text."}}) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.category = "hud";
        node.title = title;
        node.description = description;
        node.type = ConfigControlTypeV2::Toggle;
        node.defaultValue = schemaCurrentValue(initialDefaults, key, "true");
        node.currentValue = schemaCurrentValue(config, key, "true");
        node.hasCurrentValue = config.contains(key);
        schema.node(std::move(node));
    }

    setConfigSchemaJson(moduleId, schema.toJson());
}

class ModuleConfigSchemaRefreshQueue final {
public:
    ModuleConfigSchemaRefreshQueue() : m_worker(&ModuleConfigSchemaRefreshQueue::run, this) {}
    ~ModuleConfigSchemaRefreshQueue() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_condition.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }

    void schedule(std::string moduleId, nlohmann::json snapshot, std::chrono::milliseconds delay) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto now = std::chrono::steady_clock::now();
            auto it = m_pending.find(moduleId);
            if (it != m_pending.end()) {
                // Keep the longer delay in a burst that includes text input, and
                // always replace the snapshot with the newest accepted value.
                delay = std::max(delay, it->second.delay);
            }
            m_pending[std::move(moduleId)] = Pending{now + delay, delay, std::move(snapshot)};
        }
        m_condition.notify_all();
    }

private:
    struct Pending {
        std::chrono::steady_clock::time_point deadline;
        std::chrono::milliseconds delay;
        nlohmann::json snapshot;
    };

    void run() {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_stopping) {
            if (m_pending.empty()) {
                m_condition.wait(lock, [this] { return m_stopping || !m_pending.empty(); });
                continue;
            }

            auto earliest = std::min_element(m_pending.begin(), m_pending.end(),
                [](const auto& left, const auto& right) {
                    return left.second.deadline < right.second.deadline;
                });
            const auto deadline = earliest->second.deadline;
            m_condition.wait_until(lock, deadline);
            if (m_stopping) break;

            const auto now = std::chrono::steady_clock::now();
            std::vector<std::pair<std::string, nlohmann::json>> ready;
            for (auto it = m_pending.begin(); it != m_pending.end();) {
                if (it->second.deadline <= now) {
                    ready.emplace_back(it->first, std::move(it->second.snapshot));
                    it = m_pending.erase(it);
                } else {
                    ++it;
                }
            }
            lock.unlock();
            for (const auto& entry : ready) {
                if (entry.first == "bedrocktools.Aim Assist") {
                    registerAimAssistConfigSchema(entry.first, entry.second);
                } else if (entry.first == "bedrocktools.Hitbox Expander") {
                    registerHitboxExpanderConfigSchema(entry.first, entry.second);
                } else if (entry.first == "bedrocktools.ULTRAKILL HUD") {
                    registerUltrakillHudConfigSchema(entry.first, entry.second);
                }
            }
            lock.lock();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::unordered_map<std::string, Pending> m_pending;
    bool m_stopping = false;
    std::thread m_worker;
};

static void scheduleModuleConfigSchemaRefresh(Module* mod, std::string_view key,
                                               const nlohmann::json& snapshot) {
    if (!mod || (mod->moduleId != "bedrocktools.Aim Assist" &&
                 mod->moduleId != "bedrocktools.Hitbox Expander" &&
                 mod->moduleId != "bedrocktools.ULTRAKILL HUD")) return;
    // The V2 settings UI polls schema revisions, then rebuilds controls when they
    // change. Debounce continuous sliders and give the whitelist editor more time
    // so one character/drag does not reset the active widget mid-interaction.
    const auto delay = key == "m_whitelistNames"
        ? std::chrono::milliseconds(3000)
        : std::chrono::milliseconds(1200);
    static ModuleConfigSchemaRefreshQueue queue;
    queue.schedule(mod->moduleId, snapshot, delay);
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

            if (mod->moduleId == "bedrocktools.Auto Totem") {
                if (k == "m_delayMs") displayName = "AUTO TOTEM > Refill delay (ms)";
                else if (k == "m_scanRetryIntervalMs") displayName = "AUTO TOTEM > Inventory scan retry (ms)";
                else if (k == "m_offhandPollIntervalMs") displayName = "AUTO TOTEM > Offhand check interval (ms)";
                else if (k == "m_uiStepDelayMs") displayName = "AUTO TOTEM > UI step delay (ms)";
                else if (k == "m_useInventoryUiSequence") displayName = "AUTO TOTEM > Open/equip/close inventory";
                else if (k == "m_combatOnly") displayName = "AUTO TOTEM > Combat only";
                else if (k == "m_keepTotemEquipped") displayName = "AUTO TOTEM > Keep totem equipped";
            }
            if (mod->moduleId == "bedrocktools.Mace Swapper") {
                if (k == "m_enableMaceSwapper") displayName = "MACEFALL > Enable";
                else if (k == "m_attackRange") displayName = "MACEFALL > Attack range";
                else if (k == "m_minimumFallDistance") displayName = "MACEFALL > Minimum fall distance (blocks)";
                else if (k == "m_attackIntervalMs") displayName = "MACEFALL > Attack interval (ms)";
                else if (k == "m_restoreSlot") displayName = "MACEFALL > Restore previous slot";
            }
            if (mod->moduleId == "bedrocktools.Sword Swapper") {
                if (k == "m_attackRange") displayName = "BLADESHIFT > Attack range";
                else if (k == "m_restoreSlot") displayName = "BLADESHIFT > Restore previous slot";
            }
            if (mod->moduleId == "bedrocktools.Hitbox Expander") {
                if (k == "m_hitboxWidth") displayName = "BEDROCK BOUNDS > Width / depth (blocks)";
                else if (k == "m_hitboxHeight") displayName = "BEDROCK BOUNDS > Height (blocks)";
                else if (k == "m_expandPlayers") displayName = "BEDROCK BOUNDS > Players";
                else if (k == "m_expandMobs") displayName = "BEDROCK BOUNDS > Mobs";
                else if (k == "m_requireLineOfSight") displayName = "BEDROCK BOUNDS > Require line of sight";
            }

            if (mod->moduleId == "bedrocktools.Aim Assist") {
                if (k == "m_aimMode") displayName = "BEDROCK AIM > Profile";
                else if (k == "m_trackingMode") displayName = "BEDROCK AIM > Tracking mode";
                else if (k == "m_naturalization") displayName = "BEDROCK AIM > Naturalization during jumps";
                else if (k == "m_naturalizationWeapon") displayName = "BEDROCK AIM > Naturalization weapon";
                else if (k == "m_fov") displayName = "BEHAVIOR > FOV";
                else if (k == "m_targetSwitchHysteresis") displayName = "TARGETING > Crosshair switch stability";
                else if (k == "m_targetSwitchDistanceMargin") displayName = "TARGETING > Distance switch margin";
                else if (k == "m_targetSelectionMode") displayName = "TARGETING > Target selection mode";
                else if (k == "m_targetRetentionRadius") displayName = "TARGETING > Around-player lock radius (blocks)";
                else if (k == "m_autoHit") displayName = "AUTO HIT > Enable";
                else if (k == "m_autoHitRange") displayName = "AUTO HIT > Range (blocks)";
                else if (k == "m_autoHitDelayMs") displayName = "AUTO HIT > Attack interval (ms)";
                else if (k == "m_autoHitKeyBinding") displayName = "AUTO HIT > Input key (button1 / shift / 0)";
                else if (k == "m_range") displayName = "BEHAVIOR > Range";
                else if (k == "m_aimSpeed") displayName = "BEHAVIOR > Aim speed";
                else if (k == "m_rotationSpeed") displayName = "BEHAVIOR > Rotation speed";
                else if (k == "m_strength") displayName = "BEHAVIOR > Strongness";
                else if (k == "m_hitboxScale") displayName = "BEHAVIOR > Hitbox area";
                else if (k == "m_distanceDependentHitbox") displayName = "BEHAVIOR > Distance-dependent hitbox";
                else if (k == "m_hitboxDistanceGrowth") displayName = "BEHAVIOR > Distance hitbox strength (0.1-200)";
                else if (k == "m_targetPlayers") displayName = "TARGETING > Players";
                else if (k == "m_targetMobs") displayName = "TARGETING > Mobs";
                else if (k == "m_ignoreCreativeSpectator") displayName = "TARGETING > Ignore Creative/Spectator";
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
                    if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "mautohitkeycode") {
                        maxVal = 288;
                    } else if (kLower.find("cps") != std::string::npos) {
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
                if (mod->moduleId == "bedrocktools.Auto Totem" && kLower == "mscanretryintervalms") {
                    minVal = 400.0f;
                    maxVal = 2000.0f;
                } else if (mod->moduleId == "bedrocktools.Auto Totem" && kLower == "moffhandpollintervalms") {
                    minVal = 150.0f;
                    maxVal = 500.0f;
                } else if (mod->moduleId == "bedrocktools.Auto Totem" && kLower == "muiStepDelayMs") {
                    minVal = 0.0f;
                    maxVal = 500.0f;
                } else if (mod->moduleId == "bedrocktools.Hitbox Expander" && kLower == "m_hitboxsize") {
                    minVal = 1.0f;
                    maxVal = 200.0f;
                } else if (mod->moduleId == "bedrocktools.Mace Swapper" && kLower == "m_attackrange") {
                    minVal = 2.0f;
                    maxVal = 6.0f;
                } else if (mod->moduleId == "bedrocktools.Sword Swapper" && kLower == "m_attackrange") {
                    minVal = 1.0f;
                    maxVal = 16.0f;
                } else if (mod->moduleId == "bedrocktools.Mace Swapper" && kLower == "m_minimumfalldistance") {
                    minVal = 0.30f;
                    maxVal = 10.0f;
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
                    minVal = 0.30f;
                    maxVal = 50.0f;
                } else if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_hitboxdistancegrowth") {
                    minVal = 0.1f;
                    maxVal = 200.0f;
                } else if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_rotationspeed") {
                    minVal = 0.1f;
                    maxVal = 200.0f;
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
                        "All", "Sword", "Axe", "Mace", "Bow", "Crossbow", "Trident", "Spear", "Other melee", "Tools"
                    };
                    const auto selected = std::find(weapons.begin(), weapons.end(), str);
                    entry.default_value = selected == weapons.end()
                        ? "0" : std::to_string(std::distance(weapons.begin(), selected));
                    entry.min_value = "All,Sword,Axe,Mace,Bow,Crossbow,Trident,Spear,Other melee,Tools";

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

        if (mod->moduleId == "bedrocktools.Mace Swapper") {
            const std::vector<std::string> priority = {
                "m_enableMaceSwapper", "m_minimumFallDistance", "m_attackRange", "m_attackIntervalMs", "m_restoreSlot"
            };
            const auto rank = [&priority](const std::string& key) {
                const auto it = std::find(priority.begin(), priority.end(), key);
                return static_cast<std::size_t>(std::distance(priority.begin(), it));
            };
            std::stable_sort(configs.begin(), configs.end(),
                [&rank](const TmpConfigEntry& a, const TmpConfigEntry& b) {
                    return rank(a.key) < rank(b.key);
                });
        } else if (mod->moduleId == "bedrocktools.Sword Swapper") {
            const std::vector<std::string> priority = {"m_attackRange", "m_restoreSlot"};
            const auto rank = [&priority](const std::string& key) {
                const auto it = std::find(priority.begin(), priority.end(), key);
                return static_cast<std::size_t>(std::distance(priority.begin(), it));
            };
            std::stable_sort(configs.begin(), configs.end(),
                [&rank](const TmpConfigEntry& a, const TmpConfigEntry& b) {
                    return rank(a.key) < rank(b.key);
                });
        }

        if (mod->moduleId == "bedrocktools.Aim Assist") {
            // Keep the flat launcher menu visually grouped by consistent prefixes
            // and a deliberate order; the current public menu API has no section header.
            const std::vector<std::string> priority = {
                "m_aimMode", "m_trackingMode", "m_naturalization", "m_naturalizationWeapon",
                "m_fov", "m_range", "m_aimSpeed", "m_rotationSpeed", "m_predictionMode", "m_predictionLeadMs",
                "m_strength", "m_hitboxScale",
                "m_distanceDependentHitbox", "m_hitboxDistanceGrowth",
                "m_targetSwitchHysteresis", "m_targetSwitchDistanceMargin", "m_targetSelectionMode",
                "m_targetRetentionRadius", "m_autoHit", "m_autoHitRange", "m_autoHitDelayMs", "m_autoHitKeyBinding", "m_targetPlayers", "m_targetMobs", "m_cantSeeThroughBlocks", "m_whitelistNames",
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
            // Keep the growth slider present at all times; disabling dynamic hitboxes
            // changes its effect, not whether the user can see/configure the value.
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

        if (builder.registerModule()) {
            mod->onMenuRegistered();
            if (mod->moduleId == "bedrocktools.Aim Assist") {
                registerAimAssistConfigSchema(mod->moduleId, j);
            } else if (mod->moduleId == "bedrocktools.Hitbox Expander") {
                registerHitboxExpanderConfigSchema(mod->moduleId, j);
            } else if (mod->moduleId == "bedrocktools.ULTRAKILL HUD") {
                registerUltrakillHudConfigSchema(mod->moduleId, j);
            }
        }
    }
}
