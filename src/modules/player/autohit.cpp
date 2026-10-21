#include "autohit.hpp"
#include "core/VerifiedLocalPlayerTickEvent.hpp"
#include "aimassist.hpp"
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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
             "Uses the validated native GameMode attack route first, with a queued engine input callback fallback.") {
    keybind = 0;
}

void AutoHitModule::onInit() {
    bedrocktools::core::InventoryAccess::get().initialize();
    bedrocktools::events::bus().subscribe<bedaim::events::VerifiedLocalPlayerTickEvent>(
        [this](auto& event) {
            if (enabled && event.player) processPlayerTick(event.player);
        });
    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>(
        [this](auto& event) {
            const bool opened = event.phase == bedrocktools::events::ScreenPhase::Opened;
            if (event.screen == bedrocktools::events::ScreenKind::Container)
                m_containerOpen.store(opened, std::memory_order_release);
            else if (event.screen == bedrocktools::events::ScreenKind::Chat)
                m_chatOpen.store(opened, std::memory_order_release);
        });
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "initialized", "validated-native-first attack pipeline initialized");
}

void AutoHitModule::onEnable() {
    bedrocktools::core::gamehooks::cancelQueuedAttackInput();
    bedrocktools::core::InventoryAccess::get().initialize();
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_nextAttack = {};
    m_nextNativeRouteProbe = {};
    m_noHitFeedbackGuard.reset();
    m_lastGateDecision.store(-1, std::memory_order_release);
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "enabled");
}

void AutoHitModule::onDisable() {
    bedrocktools::core::gamehooks::cancelQueuedAttackInput();
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_nextAttack = {};
    m_nextNativeRouteProbe = {};
    m_noHitFeedbackGuard.reset();
    m_lastGateDecision.store(-1, std::memory_order_release);
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoHit", "lifecycle", "disabled");
}

bool AutoHitModule::onKeyEvent(int, bool, unsigned int) {
    return false;
}

bool AutoHitModule::onMouseEvent(int, bool) {
    return false;
}

void AutoHitModule::processPlayerTick(Player* player) {
    using namespace bedrocktools;

    const auto reportGate = [this](int code, const char* reason, const std::string& detail) {
        // Cooldown runs on most ticks; avoid per-tick diagnostic allocations and logs.
        if (code == 110) return;
        const int previous = m_lastGateDecision.exchange(code, std::memory_order_acq_rel);
        if (previous != code)
            diagnostics::probe(diagnostics::Level::Debug, "AutoHit", "pipeline", reason, detail);
    };

    if (!enabled) return;
    if (!player) {
        reportGate(2, "local_player_missing", "");
        return;
    }
    if (m_containerOpen.load(std::memory_order_acquire) ||
        m_chatOpen.load(std::memory_order_acquire)) {
        reportGate(3, "blocked_by_screen", "");
        return;
    }

    float configuredRange = 3.0f;
    bool swordEnabled = true, axeEnabled = false, maceEnabled = false, tridentEnabled = false;
    float swordCps = 8.0f, axeCps = 4.0f, maceCps = 2.0f, tridentCps = 3.0f;
    const auto now = std::chrono::steady_clock::now();
    bool cooldownActive = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        configuredRange = m_attackRange;
        swordEnabled = m_swordEnabled; swordCps = m_swordCps;
        axeEnabled = m_axeEnabled; axeCps = m_axeCps;
        maceEnabled = m_maceEnabled; maceCps = m_maceCps;
        tridentEnabled = m_tridentEnabled; tridentCps = m_tridentCps;
        cooldownActive = m_nextAttack.time_since_epoch().count() != 0 && now < m_nextAttack;
    }
    if (cooldownActive) {
        reportGate(110, "cooldown_waiting", "");
        return;
    }

    // Read the selected weapon only after the CPS gate opens; profile filters never
    // dispatch for unknown items or for disabled weapon categories.
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

    // Prefer the exact validated GameMode::attack vtable route. This is the
    // direct native operation, not a fake LMB telemetry event. If the public
    // attack ABI cannot be validated on this build, queue the normal engine input
    // callback for the next LocalPlayer tick instead.
    void* client = core::gamehooks::clientInstance();
    void* gameMode = nullptr;
    bool nativeRoute = false;
    bool inputRoute = false;
    bool nativeProbeBackoff = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        nativeProbeBackoff = m_nextNativeRouteProbe.time_since_epoch().count() != 0 &&
            now < m_nextNativeRouteProbe;
    }

    if (!nativeProbeBackoff) {
        gameMode = core::gamehooks::gameModeForPlayer(player);
        nativeRoute = gameMode && core::gamehooks::attackAvailable(gameMode);
        if (!nativeRoute) {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_nextNativeRouteProbe = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
    }
    if (!nativeRoute)
        inputRoute = client && core::gamehooks::inputAttackCallbackAvailable(client, player);

    if (!nativeRoute && !inputRoute) {
        reportGate(11, nativeProbeBackoff ? "attack_route_retry_backoff" : "attack_route_unavailable",
            std::string("native_route=") + (nativeProbeBackoff ? "backoff" : "unavailable") +
            "; input_callback=" + (client ? "unavailable_or_unverified" : "client_missing") +
            "; retry_in_ms=5000");
        return;
    }
    if (nativeRoute) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_nextNativeRouteProbe = {};
    }

    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (!aimBase) {
        reportGate(5, "target_provider_unavailable", "BedrockAim module is not registered");
        return;
    }
    auto* aim = static_cast<AimAssistModule*>(aimBase);
    void* target = nullptr;
    AABB targetBounds{};
    float aimRange = 0.0f;
    if (!aim->currentTargetSnapshot(target, targetBounds, aimRange, player) || !target) {
        reportGate(6, "target_snapshot_missing_or_stale", "");
        return;
    }

    // Revalidate the cached actor against a fresh engine-owned actor list before
    // any native target dereference. This occurs at most once per CPS interval.
    AABB liveBounds{};
    float liveRange = 0.0f;
    if (!aim->targetBoundsSnapshot(target, liveBounds, liveRange,
            std::clamp(configuredRange + 2.0f, 4.0f, 8.0f)) || !validBounds(liveBounds)) {
        reportGate(7, "target_not_in_live_actor_snapshot", "target is absent from the current actor snapshot");
        return;
    }
    targetBounds = liveBounds;
    aimRange = std::min(aimRange, liveRange);
    if (!aim->currentTargetInCrosshair(player, target, 0.12f)) {
        reportGate(12, "target_not_under_crosshair", "attack skipped because the current ray misses the target");
        return;
    }

    auto* expanderBase = ModuleRegistry::get().find("bedrocktools.Hitbox Expander");
    if (expanderBase && expanderBase->enabled) {
        auto* expander = static_cast<HitboxExpanderModule*>(expanderBase);
        AABB originalBounds{};
        if (!expander->originalTargetBounds(target, originalBounds) || !validBounds(originalBounds)) {
            reportGate(13, "hitbox_original_bounds_unavailable",
                "attack skipped because the expander's original target bounds are not trustworthy");
            return;
        }
        targetBounds = originalBounds;
    }

    const Vec3 eye = eyePosition(player);
    if (!finiteVec(eye)) {
        reportGate(9, "player_eye_invalid", "");
        return;
    }
    const Vec3 hitPoint{
        std::clamp(eye.x, targetBounds.min.x, targetBounds.max.x),
        std::clamp(eye.y, targetBounds.min.y, targetBounds.max.y),
        std::clamp(eye.z, targetBounds.min.z, targetBounds.max.z)
    };
    const float distanceSq = distanceSquared(eye, hitPoint);
    const float maxRange = std::clamp(std::min(configuredRange, aimRange), 2.0f, 6.0f);
    if (!finiteVec(hitPoint) || !std::isfinite(distanceSq) || distanceSq < 0.0f ||
        std::sqrt(std::max(distanceSq, 0.0f)) > maxRange) {
        reportGate(109, "target_out_of_range", "");
        return;
    }

    // Reserve exactly one attack slot; no native calls run while the mutex is held.
    const int intervalMs = bedaim::autohit::weapon::attemptIntervalMs(profile.cps);
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        const auto reserveAt = std::chrono::steady_clock::now();
        if (m_nextAttack.time_since_epoch().count() != 0 && reserveAt < m_nextAttack) {
            reportGate(110, "cooldown_waiting", "");
            return;
        }
        m_nextAttack = reserveAt + std::chrono::milliseconds(std::clamp(intervalMs, 50, 1000));
    }

    bool nativeResult = false;
    bool nativeResultUsed = false;
    const bool invoked = inputRoute
        ? core::gamehooks::queueAttackInputCallback(client, player, target)
        : core::gamehooks::attack(gameMode, target, true, &hitPoint, &nativeResult, &nativeResultUsed);

    if (!invoked) {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_nextNativeRouteProbe = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        reportGate(14, "attack_dispatch_rejected",
            std::string("route=") + (nativeRoute ? "validated_GameMode" : "queued_ClientInputCallbacks") +
            "; native_result=" + (nativeResultUsed ? (nativeResult ? "true" : "false") : "not_applicable"));
        return;
    }

    reportGate(201, nativeRoute ? "native_attack_accepted_not_damage_confirmed"
                                : "input_callback_queued_not_hit_confirmed",
        std::string("weapon=") + bedaim::autohit::weapon::name(weapon) +
        "; cps=" + std::to_string(profile.cps) + "; range=" + std::to_string(maxRange));
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
