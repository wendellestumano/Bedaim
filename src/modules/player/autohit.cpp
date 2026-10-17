#include "autohit.hpp"

#include "aimassist.hpp"
#include "AutoHitGuards.hpp"
#include "../../core/Diagnostics.hpp"
#include "core/GameHooks.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/events/ScreenStateEvent.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>
#include <utility>

namespace {
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
             "Experimental held-attack helper; requires BedrockAim to provide a fresh target.") {
    keybind = 0;
}

void AutoHitModule::onInit() {
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
                    m_heldButton = -1;
                }
            } else if (event.screen == bedrocktools::events::ScreenKind::Chat) {
                const bool opened = event.phase == bedrocktools::events::ScreenPhase::Opened;
                m_chatOpen.store(opened, std::memory_order_release);
                if (opened) {
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    m_attackHeld = false;
                    m_heldButton = -1;
                }
            }
        });
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "initialized",
        "held-input mode; module is disabled by default in the isolated test branch");
}

void AutoHitModule::onEnable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackHeld = false;
    m_heldButton = -1;
    m_nextAttack = {};
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "enabled");
}

void AutoHitModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackHeld = false;
    m_heldButton = -1;
    m_nextAttack = {};
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "disabled");
}

bool AutoHitModule::onMouseEvent(int button, bool isDown) {
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Trace,
        "AutoHit", "raw_mouse_input",
        std::string("button_") + std::to_string(button) + (isDown ? "_down" : "_up"),
        "raw button index observed by the launcher input bridge");
    std::lock_guard<std::mutex> lock(m_stateMutex);
    const bool screenBlocked = m_containerOpen.load(std::memory_order_acquire) ||
        m_chatOpen.load(std::memory_order_acquire);
    if (screenBlocked) {
        m_attackHeld = false;
        m_heldButton = -1;
        return false;
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
    int attackButton = 0;
    float configuredRange = 3.2f;
    float intervalMs = 180.0f;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        attackHeld = m_attackHeld;
        attackButton = m_attackButton;
        configuredRange = m_attackRange;
        intervalMs = m_attackIntervalMs;
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
    if (!attackHeld) {
        diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "dispatch_gate", "waiting_for_attack_input",
            std::string("configured_button=") + std::to_string(attackButton));
        return;
    }

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

    void* gameMode = core::gamehooks::gameModeForPlayer(player);
    if (!gameMode) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "gamemode", "unavailable");
        return;
    }
    const bool attackRouteAvailable = core::gamehooks::attackAvailable(gameMode);
    if (!attackRouteAvailable) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit", "attack_route", "unavailable");
        return;
    }
    diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "attack_route", "ready");

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

    const bool attacked = core::gamehooks::attack(gameMode, target, true, &hitPoint);
    if (attacked) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_nextAttack = now + std::chrono::milliseconds(
            static_cast<int>(std::clamp(intervalMs, 80.0f, 1000.0f)));
    } else {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_nextAttack = now + std::chrono::milliseconds(100);
    }
    diagnostics::probe(attacked ? diagnostics::Level::Info : diagnostics::Level::Warning,
        "AutoHit", "attack_result", attacked ? "accepted" : "rejected",
        attacked ? "native attack wrapper returned success" : "native attack wrapper returned false");
}

void AutoHitModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackButton = std::clamp(json.value("m_attackButton", m_attackButton), 0, 16);
    m_attackRange = std::clamp(json.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
    m_attackIntervalMs = std::clamp(json.value("m_attackIntervalMs", m_attackIntervalMs), 80.0f, 1000.0f);
}

void AutoHitModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    json["m_attackButton"] = m_attackButton;
    json["m_attackRange"] = m_attackRange;
    json["m_attackIntervalMs"] = m_attackIntervalMs;
}

void AutoHitModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("combat")
        .category("combat", "Combat", "Experimental held-input attack");
    auto buttonNode = [&](const char* key, const char* title, const char* minimum,
                          const char* maximum, const char* step, const char* unit) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.title = title;
        node.category = "combat";
        node.type = ConfigControlTypeV2::SliderFloat;
        node.minValue = minimum;
        node.maxValue = maximum;
        node.step = step;
        node.unit = unit;
        schema.node(std::move(node));
    };
    buttonNode("m_attackButton", "Mouse button index", "0", "16", "1", "");
    buttonNode("m_attackRange", "Attack range", "2", "6", "0.1", " blocks");
    buttonNode("m_attackIntervalMs", "Attack interval", "80", "1000", "10", " ms");
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
    try {
        const std::string raw(value);
        std::size_t parsed = 0;
        const float number = std::stof(raw, &parsed);
        while (parsed < raw.size() && std::isspace(static_cast<unsigned char>(raw[parsed]))) ++parsed;
        if (parsed != raw.size() || !std::isfinite(number)) return false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (key == "m_attackButton") m_attackButton = std::clamp(static_cast<int>(std::lround(number)), 0, 16);
        else if (key == "m_attackRange") m_attackRange = std::clamp(number, 2.0f, 6.0f);
        else if (key == "m_attackIntervalMs") m_attackIntervalMs = std::clamp(number, 80.0f, 1000.0f);
        else return false;
        return true;
    } catch (...) {
        return false;
    }
}
