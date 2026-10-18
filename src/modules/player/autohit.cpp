#include "autohit.hpp"

#include "aimassist.hpp"
#include "AutoHitGuards.hpp"
#include "AutoHitWeaponProfile.hpp"
#include "../../core/Diagnostics.hpp"
#include "core/GameHooks.hpp"
#include "core/InventoryAccess.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/events/GameModeActionEvent.hpp>
#include <bedrocktools/events/ScreenStateEvent.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <pl/ModMenu.hpp>
#include <pl/Input.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

namespace {
thread_local bool g_autoHitSyntheticAttack = false;

std::string inputLabel(int keyCode, int mouseButton, unsigned int unicodeChar = 0) {
    if (keyCode >= 0) {
        // Prefer the actual character reported by Levi's input event. Numeric
        // key codes are backend-specific: code 7 is not universally the '0' key.
        if (unicodeChar >= 32 && unicodeChar <= 126) {
            return std::string(1, static_cast<char>(unicodeChar)) + " (code " + std::to_string(keyCode) + ")";
        }
        switch (keyCode) {
            case 19: return "Arrow Up (code 19)";
            case 20: return "Arrow Down (code 20)";
            case 21: return "Arrow Left (code 21)";
            case 22: return "Arrow Right (code 22)";
            case 59: return "Left Shift (code 59)";
            case 60: return "Right Shift (code 60)";
            case 66: return "Enter (code 66)";
            case 67: return "Backspace (code 67)";
            case 111: return "Escape (code 111)";
            case 112: return "Delete (code 112)";
            default: return "Key code " + std::to_string(keyCode);
        }
    }
    switch (mouseButton) {
        case 0: return "Mouse Left";
        case 1: return "Mouse Right";
        case 2: return "Mouse Middle";
        default: return mouseButton >= 0 ? "Mouse Button " + std::to_string(mouseButton) : "None (null)";
    }
}

using bedrocktools::sdk::AABB;
using bedrocktools::sdk::Player;
using bedrocktools::sdk::Vec3;

bool finiteVec(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool validBounds(const AABB& bounds) {
    if (!finiteVec(bounds.min) || !finiteVec(bounds.max)) return false;
    const float width = bounds.max.x - bounds.min.x;
    const float height = bounds.max.y - bounds.min.y;
    const float depth = bounds.max.z - bounds.min.z;
    return width > 0.0f && width <= 5.0f && height >= 0.25f && height <= 8.0f &&
           depth > 0.0f && depth <= 5.0f;
}

float distanceSquared(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

Vec3 eyePosition(Player* player) {
    const Vec3 position = player->position();
    const AABB bounds = player->bounds();
    if (validBounds(bounds)) {
        return {(bounds.min.x + bounds.max.x) * 0.5f,
                bounds.min.y + (bounds.max.y - bounds.min.y) * 0.85f,
                (bounds.min.z + bounds.max.z) * 0.5f};
    }
    Vec3 eye = position;
    eye.y += 1.5f;
    return eye;
}

}

AutoHitModule::AutoHitModule()
    : Module("Auto Hit",
             "Experimental held-attack helper; uses the player tick path and requires a fresh BedrockAim target plus a validated native attack route.") {
    keybind = 0;
}

void AutoHitModule::onInit() {
    bedrocktools::core::InventoryAccess::get().initialize();
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { processPlayerTick(event.player); });
    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>(
        [this](auto& event) {
            if (event.screen == bedrocktools::events::ScreenKind::Container) {
                const bool opened = event.phase == bedrocktools::events::ScreenPhase::Opened;
                m_containerOpen.store(opened, std::memory_order_release);
                if (opened) {
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    m_attackHeld = false;
                    m_keyboardAttackHeld = false;
                    m_heldButton = -1;
                    m_touchRepeatUntil = {};
                }
            } else if (event.screen == bedrocktools::events::ScreenKind::Chat) {
                const bool opened = event.phase == bedrocktools::events::ScreenPhase::Opened;
                m_chatOpen.store(opened, std::memory_order_release);
                if (opened) {
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    m_attackHeld = false;
                    m_keyboardAttackHeld = false;
                    m_heldButton = -1;
                    m_touchRepeatUntil = {};
                }
            }
        });
    pl::input::registerKeyCallback([this](const pl::input::KeyEvent& input) {
        return onKeyEvent(input.keyCode, input.isKeyDown, input.unicodeChar);
    });
    // The launcher mouse callback may not receive Bedrock's native touchscreen
    // attack button. Observe ordinary engine attack actions as a mobile-compatible
    // activation signal, then permit a bounded repeat window. Synthetic attacks
    // are explicitly ignored so AutoHit cannot re-arm itself.
    bedrocktools::events::bus().subscribe<bedrocktools::events::GameModeActionEvent>(
        [this](auto& event) {
            const bool nativeAttack = event.action == bedrocktools::events::GameModeAction::Attack;
            const bool itemAsAttack = event.action == bedrocktools::events::GameModeAction::UseItemAsAttack;
            if (!nativeAttack && !itemAsAttack) return;

            const char* actionName = nativeAttack ? "Attack" : "UseItemAsAttack";
            if (!enabled) return;
            if (g_autoHitSyntheticAttack) {
                bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Trace,
                    "AutoHit", "touch_activation", "synthetic_action_ignored", actionName);
                return;
            }
            if (m_containerOpen.load(std::memory_order_acquire) ||
                m_chatOpen.load(std::memory_order_acquire)) {
                bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
                    "AutoHit", "touch_activation", "blocked_by_screen", actionName);
                return;
            }

            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (!m_touchRepeatEnabled) {
                bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
                    "AutoHit", "touch_activation",
                    nativeAttack ? "attack_seen_repeat_disabled" : "item_as_attack_seen_repeat_disabled",
                    "native action was observed, but touchscreen repeat is disabled");
                return;
            }
            const auto window = std::chrono::milliseconds(std::clamp(m_touchRepeatWindowMs, 100, 1000));
            m_touchRepeatUntil = std::chrono::steady_clock::now() + window;
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
                "AutoHit", "touch_input",
                nativeAttack ? "attack_repeat_window_armed" : "item_as_attack_repeat_window_armed",
                std::string("source=") + actionName + "; window_ms=" + std::to_string(window.count()));
        });
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "initialized",
        "mouse-held input plus optional bounded native-attack repeat for touchscreen input");
}

void AutoHitModule::onEnable() {
    // Re-resolve selected-hotbar accessors on explicit retry. This does not
    // dispatch attacks or alter Aim Assist; it refreshes the weapon reader.
    bedrocktools::core::InventoryAccess::get().initialize();
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "inventory_reader", "retry_requested",
        "reinitialized native inventory accessors on enable");
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackHeld = false;
    m_keyboardAttackHeld = false;
    m_heldButton = -1;
    m_touchRepeatUntil = {};
    m_nextAttack = {};
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "enabled");
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "activation_mode", m_requireAttackInput ? "input_required" : "automatic_when_enabled");
}

void AutoHitModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackHeld = false;
    m_keyboardAttackHeld = false;
    m_heldButton = -1;
    m_touchRepeatUntil = {};
    m_nextAttack = {};
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "disabled");
}

bool AutoHitModule::onKeyEvent(int keyCode, bool isDown, unsigned int unicodeChar) {
    std::unique_lock<std::mutex> lock(m_stateMutex);
    if (m_captureNextInput && isDown && keyCode >= 0 &&
        std::chrono::steady_clock::now() - m_captureArmedAt >= std::chrono::milliseconds(200)) {
        m_attackKeyCode = keyCode;
        m_attackKeyUnicode = unicodeChar;
        m_captureNextInput = false;
        m_keyboardAttackHeld = false;
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
            "AutoHit", "input_capture", "keyboard_key_captured",
            std::string("label=") + inputLabel(keyCode, -1, unicodeChar) +
                "; key_code=" + std::to_string(keyCode) +
                "; unicode=" + std::to_string(unicodeChar));
        lock.unlock();
        // Schema updates can touch launcher UI state; never do that while
        // holding the module's input mutex.
        onMenuRegistered();
        return false;
    }
    if (keyCode == m_attackKeyCode && m_attackKeyCode >= 0)
        m_keyboardAttackHeld = isDown;
    return false;
}

bool AutoHitModule::onMouseEvent(int button, bool isDown) {
    std::unique_lock<std::mutex> lock(m_stateMutex);
    if (m_captureNextInput && isDown &&
        std::chrono::steady_clock::now() - m_captureArmedAt >= std::chrono::milliseconds(200)) {
        m_attackButton = std::clamp(button, 0, 16);
        m_captureNextInput = false;
        m_attackHeld = false;
        m_heldButton = -1;
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
            "AutoHit", "input_capture", "mouse_button_captured",
            std::string("button=") + std::to_string(m_attackButton));
        lock.unlock();
        onMenuRegistered();
        return false;
    }
    const bool screenBlocked = m_containerOpen.load(std::memory_order_acquire) ||
        m_chatOpen.load(std::memory_order_acquire);
    if (screenBlocked) {
        m_attackHeld = false;
        m_heldButton = -1;
        return false;
    }
    if (enabled) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Trace,
            "AutoHit", "raw_mouse_input",
            std::string("button_") + std::to_string(button) + (isDown ? "_down" : "_up"),
            "raw button index observed by the launcher input bridge");
    }
    if (!enabled) {
        if (!isDown && button == m_heldButton) {
            m_attackHeld = false;
            m_heldButton = -1;
        }
        return false; // Never consume or cancel the user's normal mouse input.
    }

    if (isDown && button == m_attackButton) {
        m_attackHeld = true;
        m_heldButton = button;
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "AutoHit", "input", "attack_button_held");
    } else if (!isDown && button == m_heldButton) {
        m_attackHeld = false;
        m_heldButton = -1;
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
            "AutoHit", "input", "attack_button_released");
    }
    return false;
}

void AutoHitModule::processPlayerTick(Player* player) {
    using namespace bedrocktools;
    bool attackHeld = false;
    bool requireAttackInput = false;
    int attackButton = 0;
    int heldButton = -1;
    bool touchRepeatEnabled = false;
    int touchWindowRemainingMs = 0;
    float configuredRange = 3.2f;
    bool swordEnabled = true, axeEnabled = false, maceEnabled = false, tridentEnabled = false;
    float swordCps = 8.0f, axeCps = 4.0f, maceCps = 2.0f, tridentCps = 3.0f;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        const auto tickNow = std::chrono::steady_clock::now();
        requireAttackInput = m_requireAttackInput;
        // On Android, the default is automatic activation when enabled, so a
        // missing launcher touch callback does not block mobile users. If the
        // user explicitly requires input, however, the repeat toggle alone is
        // not a press: a real observed input or a currently armed repeat window
        // is required. This keeps the desktop and mobile setting semantics aligned.
        const bool activeRepeatWindow = m_touchRepeatUntil.time_since_epoch().count() != 0 &&
            tickNow < m_touchRepeatUntil;
        attackHeld = !requireAttackInput || m_attackHeld || m_keyboardAttackHeld ||
            (m_touchRepeatEnabled && activeRepeatWindow);
        attackButton = m_attackButton;
        heldButton = m_heldButton;
        touchRepeatEnabled = m_touchRepeatEnabled;
        if (m_touchRepeatUntil.time_since_epoch().count() != 0 && tickNow < m_touchRepeatUntil) {
            touchWindowRemainingMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                m_touchRepeatUntil - tickNow).count());
        }
        configuredRange = m_attackRange;
        swordEnabled = m_swordEnabled;
        swordCps = m_swordCps;
        axeEnabled = m_axeEnabled;
        axeCps = m_axeCps;
        maceEnabled = m_maceEnabled;
        maceCps = m_maceCps;
        tridentEnabled = m_tridentEnabled;
        tridentCps = m_tridentCps;
    }

    if (!enabled) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "dispatch_gate", "disabled");
        return;
    }
    if (!player) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "dispatch_gate", "missing_player");
        return;
    }
    if (m_containerOpen.load(std::memory_order_acquire) ||
        m_chatOpen.load(std::memory_order_acquire)) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "dispatch_gate", "screen_open");
        return;
    }
    // Check native dispatch availability before waiting for input. Otherwise an
    // unsupported runtime looks like a broken input button forever, hiding the
    // actual blocker from diagnostics. This check never performs an attack.
    void* gameMode = core::gamehooks::gameModeForPlayer(player);
    if (!gameMode) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "gamemode", "unavailable",
            std::string("pre_tick_player_address=") + std::to_string(reinterpret_cast<std::uintptr_t>(player)));
        return;
    }
    const bool attackRouteAvailable = core::gamehooks::attackAvailable(gameMode);
    if (!attackRouteAvailable) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "attack_route", "unavailable",
            "public GameMode attack vtable/signature route failed runtime validation");
        return;
    }
    diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "attack_route", "ready");

    if (!attackHeld) {
        const std::string inputState = std::string("waiting_for_attack_input_button_") +
            std::to_string(attackButton) + (touchRepeatEnabled ? "_touch_repeat_on" : "_touch_repeat_off");
        const std::string inputDetail = std::string("configured_button=") + std::to_string(attackButton) +
            "; held_button=" + std::to_string(heldButton) +
            "; touch_repeat=" + (touchRepeatEnabled ? "enabled" : "disabled") +
            "; repeat_window_remaining_ms=" + std::to_string(touchWindowRemainingMs);
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "dispatch_gate", inputState, inputDetail);
        return;
    }

    auto& inventory = core::InventoryAccess::get();
    const std::string heldItem = inventory.selectedHotbarIdentifier(player);
    const auto weapon = bedaim::autohit::weapon::classify(heldItem);
    const auto profile = bedaim::autohit::weapon::resolve(
        weapon, swordEnabled, swordCps, axeEnabled, axeCps,
        maceEnabled, maceCps, tridentEnabled, tridentCps);
    if (!profile.enabled) {
        diagnostics::probe(diagnostics::Level::Info, "AutoHit", "weapon_profile", "blocked",
            std::string("weapon=") + bedaim::autohit::weapon::name(weapon) + "; item=" +
                (heldItem.empty() ? "empty_or_unreadable" : heldItem));
        return;
    }
    const float configuredCps = profile.cps;
    const int intervalMs = bedaim::autohit::weapon::attemptIntervalMs(configuredCps);
    diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "weapon_profile", "allowed",
        std::string("weapon=") + bedaim::autohit::weapon::name(weapon) + "; cps=" + std::to_string(configuredCps));

    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (!aimBase || !aimBase->enabled) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "dependency", "bedrockaim_disabled",
            "enable BedrockAim to provide target tracking");
        return;
    }

    auto* aim = static_cast<AimAssistModule*>(aimBase);
    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    if (!aim->currentTargetSnapshot(target, targetBounds, aimRange) || !target) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "target", "missing_or_stale");
        return;
    }
    if (!validBounds(targetBounds)) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "target", "invalid_bounds");
        return;
    }
    if (!aim->currentTargetInCrosshair(player, target)) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "crosshair", "target_not_centered");
        return;
    }
    diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "crosshair", "confirmed");

    const Vec3 eye = eyePosition(player);
    if (!finiteVec(eye)) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "hit_point", "invalid_player_eye");
        return;
    }
    const Vec3 hitPoint{
        std::clamp(eye.x, targetBounds.min.x, targetBounds.max.x),
        std::clamp(eye.y, targetBounds.min.y, targetBounds.max.y),
        std::clamp(eye.z, targetBounds.min.z, targetBounds.max.z)
    };
    const float distanceSq = distanceSquared(eye, hitPoint);
    const float maxRange = std::clamp(std::min(configuredRange, aimRange), 2.0f, 6.0f);
    if (!finiteVec(hitPoint) || !std::isfinite(distanceSq) || distanceSq > maxRange * maxRange) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "range", "out_of_range");
        return;
    }
    diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "range", "in_range");

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_nextAttack.time_since_epoch().count() != 0 && now < m_nextAttack) {
            diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "cooldown", "waiting");
            return;
        }
    }

    // Final native-free gate is shared with host tests. The earlier checks are
    // intentionally retained for precise per-stage diagnostics; this last gate
    // ensures no dispatch occurs if any prerequisite becomes invalid.
    const bedaim::autohit::Inputs dispatchFacts{
        enabled,
        player != nullptr,
        target != nullptr,
        validBounds(targetBounds),
        true, // currentTargetSnapshot rejects stale target data
        gameMode != nullptr,
        attackRouteAvailable,
        finiteVec(hitPoint),
        true, // cooldown was checked immediately before resolving GameMode
        std::sqrt(distanceSq),
        maxRange
    };
    const auto dispatchDecision = bedaim::autohit::evaluate(dispatchFacts);
    if (dispatchDecision != bedaim::autohit::Decision::Ready) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "final_guard",
            "blocked", "native dispatch prerequisites changed or failed validation");
        return;
    }

    struct SyntheticAttackGuard {
        bool previous;
        SyntheticAttackGuard() : previous(g_autoHitSyntheticAttack) { g_autoHitSyntheticAttack = true; }
        ~SyntheticAttackGuard() { g_autoHitSyntheticAttack = previous; }
    } syntheticAttackGuard;
    const bool attacked = core::gamehooks::attack(gameMode, target, true, &hitPoint);
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        // CPS is an attempt-rate cap, not only a success-rate cap. Failed native
        // dispatches must not bypass the selected weapon profile and retry at 10 Hz.
        m_nextAttack = now + std::chrono::milliseconds(
            std::clamp(intervalMs, 50, 1000));
    }
    diagnostics::probe(attacked ? diagnostics::Level::Info : diagnostics::Level::Warning,
        "AutoHit", "attack_result", attacked ? "accepted" : "rejected",
        attacked ? "native attack wrapper returned success" : "native attack wrapper returned false");
}

void AutoHitModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackButton = std::clamp(json.value("m_attackButton", m_attackButton), -1, 16);
    m_attackKeyCode = json.value("m_attackKeyCode", m_attackKeyCode);
    m_attackKeyUnicode = json.value("m_attackKeyUnicode", m_attackKeyUnicode);
    m_requireAttackInput = json.value("m_requireAttackInput", m_requireAttackInput);
    m_attackRange = std::clamp(json.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
    m_touchRepeatEnabled = json.value("m_touchRepeatEnabled", m_touchRepeatEnabled);
    m_touchRepeatWindowMs = std::clamp(json.value("m_touchRepeatWindowMs", m_touchRepeatWindowMs), 100, 1000);
    const float legacyInterval = std::clamp(json.value("m_attackIntervalMs", 180.0f), 80.0f, 1000.0f);
    const float legacySwordCps = std::clamp(1000.0f / legacyInterval, 1.0f, 20.0f);
    m_swordEnabled = json.value("m_swordEnabled", m_swordEnabled);
    m_swordCps = std::clamp(json.value("m_swordCps", legacySwordCps), 1.0f, 20.0f);
    m_axeEnabled = json.value("m_axeEnabled", m_axeEnabled);
    m_axeCps = std::clamp(json.value("m_axeCps", m_axeCps), 1.0f, 20.0f);
    m_maceEnabled = json.value("m_maceEnabled", m_maceEnabled);
    m_maceCps = std::clamp(json.value("m_maceCps", m_maceCps), 1.0f, 20.0f);
    m_tridentEnabled = json.value("m_tridentEnabled", m_tridentEnabled);
    m_tridentCps = std::clamp(json.value("m_tridentCps", m_tridentCps), 1.0f, 20.0f);
}

void AutoHitModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    json["m_attackButton"] = m_attackButton;
    json["m_attackKeyCode"] = m_attackKeyCode;
    json["m_attackKeyUnicode"] = m_attackKeyUnicode;
    json["m_requireAttackInput"] = m_requireAttackInput;
    json["m_attackRange"] = m_attackRange;
    json["m_touchRepeatEnabled"] = m_touchRepeatEnabled;
    json["m_touchRepeatWindowMs"] = m_touchRepeatWindowMs;
    json["m_attackIntervalMs"] = 1000.0f / m_swordCps; // legacy config compatibility
    json["m_swordEnabled"] = m_swordEnabled;
    json["m_swordCps"] = m_swordCps;
    json["m_axeEnabled"] = m_axeEnabled;
    json["m_axeCps"] = m_axeCps;
    json["m_maceEnabled"] = m_maceEnabled;
    json["m_maceCps"] = m_maceCps;
    json["m_tridentEnabled"] = m_tridentEnabled;
    json["m_tridentCps"] = m_tridentCps;
}

void AutoHitModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("combat")
        .category("combat", "Combat", "Experimental held-input attack and per-weapon CPS profiles")
        .category("weapon_profiles", "Weapon Profiles", "Choose which held weapons may activate AutoHit and their CPS cap");

    auto slider = [&](const char* key, const char* title, const char* category,
                      const char* minimum, const char* maximum,
                      const char* step, const char* unit) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.title = title;
        node.category = category;
        node.type = ConfigControlTypeV2::SliderFloat;
        node.minValue = minimum;
        node.maxValue = maximum;
        node.step = step;
        node.unit = unit;
        schema.node(std::move(node));
    };
    auto toggle = [&](const char* key, const char* title) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.title = title;
        node.category = "weapon_profiles";
        node.type = ConfigControlTypeV2::Toggle;
        schema.node(std::move(node));
    };

    ConfigNodeV2 captureNode;
    captureNode.id = "m_captureNextInput";
    captureNode.key = "m_captureNextInput";
    captureNode.title = "Capture next keyboard/mouse input";
    captureNode.description = std::string("Press this button, then press a key or mouse button. Current keyboard binding: ") +
        inputLabel(m_attackKeyCode, -1, m_attackKeyUnicode) + "; mouse binding: " +
        inputLabel(-1, m_attackButton) + ". Numeric codes vary by input backend; the label uses the character reported by the input event when available.";
    captureNode.category = "combat";
    captureNode.type = ConfigControlTypeV2::Button;
    captureNode.actionValue = "capture";
    schema.node(std::move(captureNode));
    ConfigNodeV2 clearKeyNode;
    clearKeyNode.id = "m_clearAttackKey";
    clearKeyNode.key = "m_clearAttackKey";
    clearKeyNode.title = "Clear keyboard binding";
    clearKeyNode.description = "Removes the recorded keyboard key without changing the mouse binding.";
    clearKeyNode.category = "combat";
    clearKeyNode.type = ConfigControlTypeV2::Button;
    clearKeyNode.actionValue = "clear";
    schema.node(std::move(clearKeyNode));
    ConfigNodeV2 clearMouseNode;
    clearMouseNode.id = "m_clearAttackMouse";
    clearMouseNode.key = "m_clearAttackMouse";
    clearMouseNode.title = "Clear mouse binding";
    clearMouseNode.description = "Resets the recorded mouse button to left mouse.";
    clearMouseNode.category = "combat";
    clearMouseNode.type = ConfigControlTypeV2::Button;
    clearMouseNode.actionValue = "clear";
    schema.node(std::move(clearMouseNode));
    ConfigNodeV2 activationNode;
    activationNode.id = "m_requireAttackInput";
    activationNode.key = "m_requireAttackInput";
    activationNode.title = "Require attack input (desktop)";
    activationNode.category = "combat";
    activationNode.type = ConfigControlTypeV2::Toggle;
    schema.node(std::move(activationNode));
    slider("m_attackRange", "Attack range", "combat", "2", "6", "0.1", " blocks");
    toggle("m_touchRepeatEnabled", "Enable touchscreen attack repeat");
    slider("m_touchRepeatWindowMs", "Touch repeat window", "combat", "100", "1000", "50", " ms");
    toggle("m_swordEnabled", "Enable sword profile");
    slider("m_swordCps", "Sword CPS cap", "weapon_profiles", "1", "20", "0.5", " CPS");
    toggle("m_axeEnabled", "Enable axe profile");
    slider("m_axeCps", "Axe CPS cap", "weapon_profiles", "1", "20", "0.5", " CPS");
    toggle("m_maceEnabled", "Enable mace profile");
    slider("m_maceCps", "Mace CPS cap", "weapon_profiles", "1", "20", "0.5", " CPS");
    toggle("m_tridentEnabled", "Enable trident profile");
    slider("m_tridentCps", "Trident CPS cap", "weapon_profiles", "1", "20", "0.5", " CPS");

    ConfigNodeV2 keybindNode;
    keybindNode.id = "keybind";
    keybindNode.key = "keybind";
    keybindNode.title = "Keybind";
    keybindNode.category = "combat";
    keybindNode.type = ConfigControlTypeV2::Keybind;
    schema.node(std::move(keybindNode));
    setConfigSchemaJson(moduleId, schema.toJson());
}

bool AutoHitModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "keybind") return false;
    if (key == "m_clearAttackKey") {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_attackKeyCode = -1;
        m_attackKeyUnicode = 0;
        m_keyboardAttackHeld = false;
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
            "AutoHit", "input_capture", "keyboard_binding_cleared");
        onMenuRegistered();
        return true;
    }
    if (key == "m_clearAttackMouse") {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_attackButton = 0;
        m_attackHeld = false;
        m_heldButton = -1;
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
            "AutoHit", "input_capture", "mouse_binding_reset", "button=0");
        onMenuRegistered();
        return true;
    }
    if (key == "m_captureNextInput") {
        if (value != "capture" && value != "true" && value != "1") return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_captureNextInput = true;
        m_captureArmedAt = std::chrono::steady_clock::now();
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
            "AutoHit", "input_capture", "armed",
            "waiting for one keyboard key or mouse button after a 200 ms arming delay");
        return true;
    }
    if (key == "m_requireAttackInput") {
        const bool on = value == "true" || value == "1" || value == "on" || value == "yes";
        const bool off = value == "false" || value == "0" || value == "off" || value == "no";
        if (!on && !off) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_requireAttackInput = on;
        if (!on) {
            m_attackHeld = false;
            m_heldButton = -1;
            m_touchRepeatUntil = {};
        }
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
            "AutoHit", "activation_mode", on ? "input_required" : "automatic_when_enabled");
        return true;
    }
    if (key == "m_touchRepeatEnabled") {
        const bool on = value == "true" || value == "1" || value == "on" || value == "yes";
        const bool off = value == "false" || value == "0" || value == "off" || value == "no";
        if (!on && !off) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_touchRepeatEnabled = on;
        if (!on) m_touchRepeatUntil = {};
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
            "AutoHit", "touch_input", on ? "enabled" : "disabled");
        return true;
    }
    if (key == "m_swordEnabled" || key == "m_axeEnabled" ||
        key == "m_maceEnabled" || key == "m_tridentEnabled") {
        const bool on = value == "true" || value == "1" || value == "on" || value == "yes";
        const bool off = value == "false" || value == "0" || value == "off" || value == "no";
        if (!on && !off) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (key == "m_swordEnabled") m_swordEnabled = on;
        else if (key == "m_axeEnabled") m_axeEnabled = on;
        else if (key == "m_maceEnabled") m_maceEnabled = on;
        else m_tridentEnabled = on;
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
            "AutoHit", "weapon_profile", std::string(key) + (on ? "_enabled" : "_disabled"));
        return true;
    }
    try {
        const std::string raw(value);
        std::size_t parsed = 0;
        const float number = std::stof(raw, &parsed);
        while (parsed < raw.size() && std::isspace(static_cast<unsigned char>(raw[parsed]))) ++parsed;
        if (parsed != raw.size() || !std::isfinite(number)) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (key == "m_attackButton") m_attackButton = std::clamp(static_cast<int>(std::lround(number)), 0, 16);
        else if (key == "m_attackRange") m_attackRange = std::clamp(number, 2.0f, 6.0f);
        else if (key == "m_touchRepeatWindowMs") m_touchRepeatWindowMs = std::clamp(static_cast<int>(std::lround(number)), 100, 1000);
        else if (key == "m_swordCps") m_swordCps = std::clamp(number, 1.0f, 20.0f);
        else if (key == "m_axeCps") m_axeCps = std::clamp(number, 1.0f, 20.0f);
        else if (key == "m_maceCps") m_maceCps = std::clamp(number, 1.0f, 20.0f);
        else if (key == "m_tridentCps") m_tridentCps = std::clamp(number, 1.0f, 20.0f);
        else return false;
        return true;
    } catch (...) {
        return false;
    }
}
