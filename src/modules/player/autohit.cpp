#include "autohit.hpp"
#include "core/VerifiedLocalPlayerTickEvent.hpp"
#include <cstdio>

#include "aimassist.hpp"
#include "AutoHitGuards.hpp"
#include "AutoHitWeaponProfile.hpp"
#include "../../core/Diagnostics.hpp"
#include "core/GameHooks.hpp"
#include "core/InventoryAccess.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/events/ScreenStateEvent.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

namespace {
thread_local bool g_autoHitSyntheticAttack = false;

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
             "Automatically attacks eligible in-range targets while enabled; no attack key or mouse binding is required.") {
    keybind = 0;
}

void AutoHitModule::onInit() {
    bedrocktools::core::InventoryAccess::get().initialize();
    // One verified post-tick callback, never one expensive AutoHit pass per actor.
    bedrocktools::events::bus().subscribe<bedaim::events::VerifiedLocalPlayerTickEvent>(
        [this](auto& event) {
            if (!enabled || !event.player) return;
            processPlayerTick(event.player);
        });
    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>(
        [this](auto& event) {
            if (event.screen == bedrocktools::events::ScreenKind::Container) {
                const bool opened = event.phase == bedrocktools::events::ScreenPhase::Opened;
                m_containerOpen.store(opened, std::memory_order_release);
                // Screen state is the only input-related pause retained.
            } else if (event.screen == bedrocktools::events::ScreenKind::Chat) {
                const bool opened = event.phase == bedrocktools::events::ScreenPhase::Opened;
                m_chatOpen.store(opened, std::memory_order_release);
                // Screen state is the only input-related pause retained.
            }
        });
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "initialized",
        "automatic attack while enabled; no attack input binding is used");
}

void AutoHitModule::onEnable() {
    // Re-resolve selected-hotbar accessors on explicit retry. This does not
    // dispatch attacks or alter Aim Assist; it refreshes the weapon reader.
    bedrocktools::core::InventoryAccess::get().initialize();
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "inventory_reader", "retry_requested",
        "reinitialized native inventory accessors on enable");
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_nextAttack = {};
    m_noHitFeedbackGuard.reset();
    m_lastGateDecision.store(-1, std::memory_order_release);
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "enabled");
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "activation_mode", "automatic_when_enabled_no_attack_binding");
}

void AutoHitModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_nextAttack = {};
    m_noHitFeedbackGuard.reset();
    m_lastGateDecision.store(-1, std::memory_order_release);
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "disabled");
}

bool AutoHitModule::onKeyEvent(int, bool, unsigned int) {
    // Attack input is deliberately not part of AutoHit's activation contract.
    return false;
}

bool AutoHitModule::onMouseEvent(int, bool) {
    // Do not consume or require launcher mouse input; module state controls activation.
    return false;
}

void AutoHitModule::processPlayerTick(Player* player) {
    using namespace bedrocktools;
    using bedaim::autohit::Decision;

    // Keep the tick path deterministic: copy user-controlled state once, then
    // perform all target/inventory/native reads without holding the input mutex.
    auto reportGate = [this](int code, const char* reason, const std::string& detail) {
        // Cooldown is expected on almost every tick between attacks; do not let
        // it alternate with "accepted" and flood the rolling diagnostic buffer.
        if (code == 110 || code == 200) return;
        const int previous = m_lastGateDecision.exchange(code, std::memory_order_acq_rel);
        if (previous != code) {
            diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "pipeline", reason, detail);
        }
    };

    if (!enabled) {
        reportGate(1, "module_disabled", "");
        return;
    }
    if (!player) {
        reportGate(2, "local_player_missing", "");
        return;
    }
    if (m_containerOpen.load(std::memory_order_acquire) ||
        m_chatOpen.load(std::memory_order_acquire)) {
        reportGate(3, "blocked_by_screen", "");
        return;
    }

    // AutoHit has no attack key/button gate: enabling the module is sufficient.
    // Copy the weapon profile and reach under one short lock, then leave the
    // render/tick-critical path free of input-capture bookkeeping.
    float configuredRange = 3.2f;
    bool swordEnabled = true, axeEnabled = false, maceEnabled = false, tridentEnabled = false;
    float swordCps = 8.0f, axeCps = 4.0f, maceCps = 2.0f, tridentCps = 3.0f;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        configuredRange = m_attackRange;
        swordEnabled = m_swordEnabled; swordCps = m_swordCps;
        axeEnabled = m_axeEnabled; axeCps = m_axeCps;
        maceEnabled = m_maceEnabled; maceCps = m_maceCps;
        tridentEnabled = m_tridentEnabled; tridentCps = m_tridentCps;
    }

    // Do not rescan targets, inspect inventory, or resolve native GameMode on
    // every local tick while the weapon's CPS timer is still active.
    bool waitingForCooldown = false;
    {
        const auto tickNow = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(m_stateMutex);
        waitingForCooldown = m_nextAttack.time_since_epoch().count() != 0 && tickNow < m_nextAttack;
    }
    if (waitingForCooldown) {
        reportGate(110, "cooldown_waiting", "");
        return;
    }

    auto& inventory = core::InventoryAccess::get();
    const std::string heldItem = inventory.selectedHotbarIdentifier(player);
    const auto weapon = bedaim::autohit::weapon::classify(heldItem);
    const auto profile = bedaim::autohit::weapon::resolve(
        weapon, swordEnabled, swordCps, axeEnabled, axeCps, maceEnabled, maceCps,
        tridentEnabled, tridentCps);
    if (!profile.enabled) {
        reportGate(8, "weapon_profile_rejected",
            std::string("identifier=") + (heldItem.empty() ? "<empty>" : heldItem) +
            "; class=" + bedaim::autohit::weapon::name(weapon));
        return;
    }

    // Resolve the native route before asking the shared target provider to scan
    // entities. If this Minecraft build cannot validate attack(), repeated 80-block
    // target scans are wasted work and were a major source of the observed lag.
    void* gameMode = core::gamehooks::gameModeForPlayer(player);
    const bool routeAvailable = gameMode && core::gamehooks::attackAvailable(gameMode);
    if (!gameMode || !routeAvailable) {
        reportGate(11, "native_attack_route_unavailable",
            std::string("game_mode=") + (gameMode ? "present" : "missing") +
            "; attack_route=" + (routeAvailable ? "ready" : "unavailable"));
        return;
    }

    // ProtoHax's useful separation is preserved here: target acquisition,
    // weapon eligibility, reach/cooldown, and native dispatch are independent
    // gates. Bedaim uses the engine's validated native route rather than
    // ProtoHax's relay/network transaction API.
    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (!aimBase) {
        reportGate(5, "aim_assist_unavailable", "AutoHit needs the shared BedrockAim target provider");
        return;
    }
    auto* aim = static_cast<AimAssistModule*>(aimBase);
    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    const bool targetFresh = aim->currentTargetSnapshot(target, targetBounds, aimRange, player);
    if (!targetFresh || !target) {
        reportGate(6, "target_snapshot_missing_or_stale",
            "no fresh target snapshot belongs to the current local player");
        return;
    }

    bool targetQuarantined = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        targetQuarantined = m_noHitFeedbackGuard.isBlocked(target);
    }
    if (targetQuarantined) {
        reportGate(203, "target_quarantined_no_hit_feedback",
            "six accepted native attempts showed no positive hurt-time transition; target dispatch is paused until target identity changes or AutoHit is toggled");
        return;
    }

    // The shared snapshot avoids a second target-selection pass. Before a native
    // call, verify that this actor still belongs to the live engine actor set and
    // refresh its bounds; this happens only when the CPS gate is open, not every tick.
    AABB liveTargetBounds{};
    float liveTargetRange = 0.0f;
    if (!aim->targetBoundsSnapshot(target, liveTargetBounds, liveTargetRange,
            std::clamp(configuredRange + 2.0f, 4.0f, 8.0f)) ||
        !validBounds(liveTargetBounds)) {
        reportGate(7, "target_not_in_live_actor_snapshot",
            "cached target could not be revalidated against the current engine actor set");
        return;
    }
    targetBounds = liveTargetBounds;
    aimRange = std::min(aimRange, liveTargetRange);

    const Vec3 eye = eyePosition(player);
    if (!finiteVec(eye)) {
        reportGate(9, "player_eye_invalid", "player eye position contains non-finite coordinates");
        return;
    }
    const Vec3 nearestPoint{
        std::clamp(eye.x, targetBounds.min.x, targetBounds.max.x),
        std::clamp(eye.y, targetBounds.min.y, targetBounds.max.y),
        std::clamp(eye.z, targetBounds.min.z, targetBounds.max.z)
    };
    const float distanceSq = distanceSquared(eye, nearestPoint);
    const float maxRange = std::clamp(std::min(configuredRange, aimRange), 2.0f, 6.0f);
    // GameMode::attack receives a hit/contact position, not merely a target
    // center. Use the closest point on the freshly validated target AABB to the
    // player's eye. The center can be behind/inside the target and is not the
    // surface point corresponding to the measured reach distance.
    const Vec3 hitPoint = nearestPoint;
    const bool hitPointFinite = finiteVec(hitPoint) && finiteVec(nearestPoint) &&
        std::isfinite(distanceSq) && distanceSq >= 0.0f;
    if (!hitPointFinite) {
        reportGate(10, "hit_geometry_invalid", "computed hit point or distance is not finite");
        return;
    }

    if (std::sqrt(std::max(distanceSq, 0.0f)) > maxRange) {
        reportGate(109, "target_out_of_range",
            std::string("distance=") + std::to_string(std::sqrt(std::max(distanceSq, 0.0f))) +
            "; range=" + std::to_string(maxRange));
        return;
    }

    // The native route was preflighted before target acquisition; re-check only
    // the cooldown immediately before reserving the CPS slot.
    const auto now = std::chrono::steady_clock::now();
    bool cooldownReady = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        cooldownReady = m_nextAttack.time_since_epoch().count() == 0 || now >= m_nextAttack;
    }

    const bedaim::autohit::Inputs gateInputs{
        true, player != nullptr, target != nullptr, validBounds(targetBounds), targetFresh,
        gameMode != nullptr, routeAvailable, hitPointFinite, cooldownReady,
        std::sqrt(std::max(distanceSq, 0.0f)), maxRange
    };
    const Decision decision = bedaim::autohit::evaluate(gateInputs);
    const auto decisionName = [](Decision value) -> const char* {
        switch (value) {
            case Decision::Ready: return "ready";
            case Decision::Disabled: return "module_disabled";
            case Decision::MissingPlayer: return "local_player_missing";
            case Decision::MissingTarget: return "target_missing";
            case Decision::InvalidBounds: return "target_bounds_invalid";
            case Decision::StaleTarget: return "target_snapshot_stale";
            case Decision::MissingGameMode: return "game_mode_unavailable";
            case Decision::MissingAttackRoute: return "native_attack_route_unavailable";
            case Decision::InvalidHitPoint: return "hit_geometry_invalid";
            case Decision::OutOfRange: return "target_out_of_range";
            case Decision::Cooldown: return "cooldown_waiting";
        }
        return "unknown_gate";
    };
    if (decision != Decision::Ready) {
        const int code = 100 + static_cast<int>(decision);
        reportGate(code, decisionName(decision),
            std::string("distance=") + std::to_string(gateInputs.distance) +
            "; range=" + std::to_string(maxRange) +
            "; game_mode=" + (gameMode ? "present" : "missing") +
            "; attack_route=" + (routeAvailable ? "ready" : "unavailable"));
        return;
    }

    const int intervalMs = bedaim::autohit::weapon::attemptIntervalMs(profile.cps);
    bool reservedAttackSlot = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        // Recheck under the same lock that reserves the slot. This makes the
        // pacing gate safe if another callback ever enters the pipeline.
        const auto reserveTime = std::chrono::steady_clock::now();
        if (m_nextAttack.time_since_epoch().count() == 0 || reserveTime >= m_nextAttack) {
            m_nextAttack = reserveTime + std::chrono::milliseconds(std::clamp(intervalMs, 50, 1000));
            reservedAttackSlot = true;
        }
    }
    if (!reservedAttackSlot) {
        reportGate(200, "cooldown_waiting", "CPS slot was reserved by another callback");
        return;
    }

    struct SyntheticAttackGuard {
        bool previous;
        SyntheticAttackGuard() : previous(g_autoHitSyntheticAttack) { g_autoHitSyntheticAttack = true; }
        ~SyntheticAttackGuard() { g_autoHitSyntheticAttack = previous; }
    } syntheticGuard;

    const int targetHurtTimeBefore =
        reinterpret_cast<bedrocktools::sdk::Actor*>(target)->hurtTime();
    const bool accepted = core::gamehooks::attack(gameMode, target, true, &hitPoint);
    // Hurt time is only an immediate local observation: server-authoritative
    // target state can arrive later, so this is evidence, not a damage verdict.
    const int targetHurtTimeAfter =
        reinterpret_cast<bedrocktools::sdk::Actor*>(target)->hurtTime();
    bool targetQuarantinedNow = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        targetQuarantinedNow = m_noHitFeedbackGuard.observe(
            target, accepted, targetHurtTimeBefore, targetHurtTimeAfter);
    }
    if (targetQuarantinedNow && accepted) {
        diagnostics::probe(diagnostics::Level::Warning, "AutoHit",
            "safety_guard", "target_quarantined_no_hit_feedback",
            "six consecutive accepted native calls did not increase local target hurt-time; dispatch to this actor is paused to avoid unbounded repetition. This is not a server-damage verdict.");
    }
    const auto pointerHex = [](const void* ptr) {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%llX",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(ptr)));
        return std::string(buffer);
    };
    reportGate(accepted ? 201 : 202,
        accepted ? "native_attack_accepted" : "native_attack_rejected",
        std::string("target=0x") + pointerHex(target) +
        "; player=0x" + pointerHex(player) +
        "; distance=" + std::to_string(gateInputs.distance) +
        "; range=" + std::to_string(maxRange) +
        "; weapon=" + bedaim::autohit::weapon::name(weapon) +
        "; cps=" + std::to_string(profile.cps) +
        "; hit_point=(" + std::to_string(hitPoint.x) + "," +
            std::to_string(hitPoint.y) + "," + std::to_string(hitPoint.z) + ")" +
        "; target_hurt_time_before=" + std::to_string(targetHurtTimeBefore) +
        "; target_hurt_time_after=" + std::to_string(targetHurtTimeAfter) +
        "; wrapper acceptance and immediate hurt-time observation do not prove server-confirmed damage");
}

void AutoHitModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackRange = std::clamp(json.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
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
    json["m_attackRange"] = m_attackRange;
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
        .category("combat", "Combat", "Automatic attack while enabled, reach and per-weapon CPS profiles")
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

    slider("m_attackRange", "Attack range", "combat", "2", "6", "0.1", " blocks");
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
        if (key == "m_attackRange") m_attackRange = std::clamp(number, 2.0f, 6.0f);
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
