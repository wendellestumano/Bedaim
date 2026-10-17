#include "swordswapper.hpp"
#include "swapperstate.hpp"
#include "../../core/Diagnostics.hpp"
#include <bedrocktools/events/GameModeActionEvent.hpp>

#include <pl/ModMenu.hpp>
#include <cctype>
#include <string_view>
#include <utility>

#include "aimassist.hpp"
#include "core/InventoryAccess.hpp"
#include "../ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>

namespace {
bool finiteVec(const bedrocktools::sdk::Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool validBounds(const bedrocktools::sdk::AABB& bounds) {
    if (!finiteVec(bounds.min) || !finiteVec(bounds.max)) return false;
    const float height = bounds.max.y - bounds.min.y;
    const float width = bounds.max.x - bounds.min.x;
    const float depth = bounds.max.z - bounds.min.z;
    return height >= 0.25f && height <= 8.0f && width > 0.0f && width <= 5.0f
        && depth > 0.0f && depth <= 5.0f;
}

bedrocktools::sdk::Vec3 eyePositionFor(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    if (!player) return {};
    const Vec3 position = player->position();
    const AABB bounds = player->bounds();
    Vec3 eye = position;
    if (validBounds(bounds)) {
        eye.x = (bounds.min.x + bounds.max.x) * 0.5f;
        eye.y = bounds.min.y + (bounds.max.y - bounds.min.y) * 0.85f;
        eye.z = (bounds.min.z + bounds.max.z) * 0.5f;
    } else {
        eye.y += 1.5f;
    }
    return eye;
}

bool targetWithinRange(bedrocktools::sdk::Player* player,
                       const bedrocktools::sdk::AABB& bounds, float range) {
    if (!player || !validBounds(bounds)) return false;
    const auto eye = eyePositionFor(player);
    if (!finiteVec(eye)) return false;
    const bedrocktools::sdk::Vec3 nearest{
        std::clamp(eye.x, bounds.min.x, bounds.max.x),
        std::clamp(eye.y, bounds.min.y, bounds.max.y),
        std::clamp(eye.z, bounds.min.z, bounds.max.z)
    };
    const float dx = eye.x - nearest.x;
    const float dy = eye.y - nearest.y;
    const float dz = eye.z - nearest.z;
    const float distanceSq = dx * dx + dy * dy + dz * dz;
    const float safeRange = std::clamp(range, 2.0f, 6.0f);
    return std::isfinite(distanceSq) && distanceSq <= safeRange * safeRange;
}
}

SwordSwapperModule::SwordSwapperModule()
    : Module("ShiftBlade", "Prioritizes a hotbar sword while a BedrockAim target is within range, yielding to consumables and manual slot choices.",
             "Sword Swapper") {
    keybind = 0;
}

void SwordSwapperModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { processPlayerTick(event.player); });
    bedrocktools::events::bus().subscribe<bedrocktools::events::GameModeActionEvent>(
        [this](auto& event) {
            if (event.action != bedrocktools::events::GameModeAction::UseItem) return;
            // A use action may start the eating animation even when the native
            // boolean result is false. Treat the action itself as the signal;
            // the tick path only applies the guard while a consumable is selected.
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_lastUseItemAction = std::chrono::steady_clock::now();
        });
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, moduleId,
        "initialized; sword retries use a 1-second cooldown and consumption guard tracks UseItem actions, including false native results");
}

void SwordSwapperModule::onEnable() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastPlayerY = 0.0f;
    m_hasLastPlayerY = false;
    m_lastGroundY = 0.0f;
    m_hasGroundY = false;
    m_groundedTicks = 0;
    m_userOverrodeSwap = false;
    m_consumptionGuardActive = false;
    m_nextSwordAttempt = {};
    m_lastUseItemAction = {};
    // Preserve ownership if the user re-enables the module before the next game
    // tick has had a chance to restore the temporarily selected sword.
}

void SwordSwapperModule::onDisable() {
    // Do not touch inventory from the UI callback thread. The next player tick
    // restores the old slot if this module still owns the sword selection.
}

void SwordSwapperModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    using namespace bedrocktools::sdk;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    auto& inventory = bedrocktools::core::InventoryAccess::get();
    auto reportStatus = [this](std::string status, bedrocktools::diagnostics::Level level = bedrocktools::diagnostics::Level::Debug) {
        if (status == m_lastStatus) return;
        m_lastStatus = status;
        bedrocktools::diagnostics::log(level, moduleId, status);
    };

    if (!player) {
        reportStatus("waiting: local player pointer unavailable", bedrocktools::diagnostics::Level::Warning);
        m_lastPlayerY = 0.0f;
        m_hasLastPlayerY = false;
        m_lastGroundY = 0.0f;
        m_hasGroundY = false;
        m_groundedTicks = 0;
        m_ownsSwap = false;
        m_swordSlot = -1;
        m_previousSlot = -1;
        return;
    }

    auto restoreIfOwned = [&]() {
        const int current = inventory.selectedHotbarSlot(player);
        if (m_ownsSwap && m_restoreSlot && current == m_swordSlot && m_previousSlot >= 0)
            inventory.selectHotbarSlot(player, m_previousSlot);
        m_ownsSwap = false;
        m_swordSlot = -1;
        m_previousSlot = -1;
    };

    if (!enabled) {
        reportStatus("inactive: module disabled");
        restoreIfOwned();
        m_userOverrodeSwap = false;
        m_consumptionGuardActive = false;
        m_nextSwordAttempt = {};
        return;
    }

    const Vec3 position = player->position();
    if (!finiteVec(position)) {
        reportStatus("blocked: player position is not finite", bedrocktools::diagnostics::Level::Warning);
        m_hasLastPlayerY = false;
        m_hasGroundY = false;
        m_groundedTicks = 0;
        restoreIfOwned();
        return;
    }
    const float dy = m_hasLastPlayerY ? position.y - m_lastPlayerY : 0.0f;
    m_lastPlayerY = position.y;
    m_hasLastPlayerY = true;
    if (!m_hasGroundY) {
        m_lastGroundY = position.y;
        m_hasGroundY = true;
    }

    // Require several stable Y samples, but compare against the last confirmed
    // ground level so pausing briefly at a jump apex is not treated as grounded.
    if (std::fabs(dy) <= 0.02f)
        m_groundedTicks = std::min(m_groundedTicks + 1, 6);
    else
        m_groundedTicks = 0;
    const float allowedGroundDistance = std::clamp(m_groundDistance, 0.0f, 3.0f);
    const float heightAboveGround = std::max(0.0f, position.y - m_lastGroundY);
    const bool groundedOrNearGround =
        m_groundedTicks >= 3 && dy >= -0.02f &&
        heightAboveGround <= allowedGroundDistance + 0.02f;
    if (groundedOrNearGround && heightAboveGround <= 0.02f)
        m_lastGroundY = position.y;

    // Never let ShiftBlade override the mace while it owns a confirmed downward slam.
    if (swapperstate::maceFallPriority.load(std::memory_order_acquire)) {
        reportStatus("paused: Mace Swapper has slam priority");
        restoreIfOwned();
        return;
    }

    if (m_ownsSwap && inventory.selectedHotbarSlot(player) != m_swordSlot) {
        // A changed slot can be a manual choice or a transient inventory selection
        // update. By default ShiftBlade keeps its sword equipped while the target is
        // in range; users who prefer manual hotbar control can opt out in settings.
        m_ownsSwap = false;
        m_swordSlot = -1;
        m_previousSlot = -1;
        if (m_respectManualSlotChanges) m_userOverrodeSwap = true;
    }

    const std::string heldIdentifier = inventory.selectedHotbarIdentifier(player);
    const bool holdingConsumable =
        heldIdentifier.find("apple") != std::string::npos ||
        heldIdentifier.find("carrot") != std::string::npos ||
        heldIdentifier.find("potato") != std::string::npos ||
        heldIdentifier.find("bread") != std::string::npos ||
        heldIdentifier.find("cookie") != std::string::npos ||
        heldIdentifier.find("cooked_") != std::string::npos ||
        heldIdentifier.find("raw_") != std::string::npos ||
        heldIdentifier.find("beef") != std::string::npos ||
        heldIdentifier.find("porkchop") != std::string::npos ||
        heldIdentifier.find("chicken") != std::string::npos ||
        heldIdentifier.find("mutton") != std::string::npos ||
        heldIdentifier.find("rabbit") != std::string::npos ||
        heldIdentifier.find("fish") != std::string::npos ||
        heldIdentifier.find("stew") != std::string::npos ||
        heldIdentifier.find("soup") != std::string::npos ||
        heldIdentifier.find("berries") != std::string::npos ||
        heldIdentifier.find("beetroot") != std::string::npos ||
        heldIdentifier.find("melon_slice") != std::string::npos ||
        heldIdentifier.find("pumpkin_pie") != std::string::npos ||
        heldIdentifier.find("dried_kelp") != std::string::npos ||
        heldIdentifier.find("honey_bottle") != std::string::npos ||
        heldIdentifier.find("milk_bucket") != std::string::npos ||
        heldIdentifier.find("potion") != std::string::npos ||
        heldIdentifier.find("rotten_flesh") != std::string::npos ||
        heldIdentifier.find("spider_eye") != std::string::npos ||
        heldIdentifier.find("chorus_fruit") != std::string::npos;

    // Merely holding food no longer blocks ShiftBlade. We pause only when the
    // native UseItem hook succeeded recently while a consumable is selected.
    const auto now = std::chrono::steady_clock::now();
    const bool activelyConsuming = holdingConsumable &&
        m_lastSuccessfulUseItem.time_since_epoch().count() != 0 &&
        now - m_lastSuccessfulUseItem < std::chrono::milliseconds(1800);
    if (activelyConsuming) {
        restoreIfOwned();
        reportStatus("paused: UseItem action detected while a consumable is selected");
        m_consumptionGuardActive = true;
        return;
    }
    if (m_consumptionGuardActive) {
        reportStatus("consumption ended; sword swap can resume");
        m_consumptionGuardActive = false;
    }
    if (heldIdentifier.find("wind_charge") != std::string::npos) {
        // Explicit user selection beats the automatic sword swap. Keep the
        // override until the target leaves the configured range.
        restoreIfOwned();
        m_userOverrodeSwap = true;
        return;
    }

    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    auto* aim = (aimBase && aimBase->enabled) ? static_cast<AimAssistModule*>(aimBase) : nullptr;
    bool hasTarget = false;
    bool targetInRange = false;

    if (groundedOrNearGround && aim) {
        void* target = nullptr;
        AABB bounds{};
        float aimRange = 0.0f;
        hasTarget = aim->currentTargetSnapshot(target, bounds, aimRange);
        targetInRange = hasTarget &&
            targetWithinRange(player, bounds, std::min(m_attackRange, aimRange));
    }

    if (!groundedOrNearGround) {
        reportStatus("waiting: player is not grounded or within allowed ground distance");
    } else if (!aim) {
        reportStatus("blocked: BedrockAim/Aim Assist is disabled or unavailable");
    } else if (!hasTarget) {
        reportStatus("waiting: BedrockAim has no valid target snapshot");
    } else if (!targetInRange) {
        reportStatus("waiting: target is outside ShiftBlade range");
    }

    if (!targetInRange || !groundedOrNearGround || !aim) {
        restoreIfOwned();
        m_userOverrodeSwap = false;
        m_nextSwordAttempt = {};
        return;
    }
    if (m_userOverrodeSwap) {
        reportStatus("paused: manual hotbar override is enabled");
        return;
    }
    reportStatus("active: valid target is in range; sword equip retry is enabled", bedrocktools::diagnostics::Level::Info);

    const int selected = inventory.selectedHotbarSlot(player);
    if (selected < 0) return;
    const int swordSlot = inventory.findHotbarSlot(player, "_sword");
    if (swordSlot < 0) {
        if (now >= m_nextSwordAttempt) {
            m_nextSwordAttempt = now + std::chrono::seconds(1);
            bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Debug, moduleId,
                "target is in range but no sword was found in the hotbar");
        }
        return;
    }
    if (selected == swordSlot) {
        m_nextSwordAttempt = now + std::chrono::seconds(1);
        return;
    }
    if (now < m_nextSwordAttempt) return;

    // Retry at most once per second while the target/ground conditions remain valid.
    m_nextSwordAttempt = now + std::chrono::seconds(1);
    bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Debug, moduleId,
        "attempting to equip sword; target range and ground conditions are valid");
    const bool selectedSword = inventory.selectHotbarSlot(player, swordSlot) &&
                               inventory.selectedHotbarSlot(player) == swordSlot;
    if (selectedSword) {
        m_ownsSwap = true;
        m_swordSlot = swordSlot;
        m_previousSlot = selected;
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Info, moduleId,
            "sword equipped successfully");
    } else {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning, moduleId,
            "failed to equip sword; retry scheduled in 1 second");
    }
}

void SwordSwapperModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_attackRange = std::clamp(j.value("m_attackRange", m_attackRange), 2.0f, 6.0f);
    m_groundDistance = std::clamp(j.value("m_groundDistance", m_groundDistance), 0.0f, 3.0f);
    m_restoreSlot = j.value("m_restoreSlot", m_restoreSlot);
    m_respectManualSlotChanges = j.value("m_respectManualSlotChanges", m_respectManualSlotChanges);
}

void SwordSwapperModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    j["m_attackRange"] = m_attackRange;
    j["m_groundDistance"] = m_groundDistance;
    j["m_restoreSlot"] = m_restoreSlot;
    j["m_respectManualSlotChanges"] = m_respectManualSlotChanges;
}

void SwordSwapperModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("combat")
        .category("combat", "Combat", "ShiftBlade targeting and hotbar behavior");

    auto slider = [&](const char* key, const char* title,
                      const char* minimum, const char* maximum,
                      const char* step, const char* unit) {
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

    slider("m_attackRange", "Target range", "2", "6", "0.1", " blocks");
    slider("m_groundDistance", "Allowed distance above ground", "0", "3", "0.05", " blocks");

    ConfigNodeV2 restoreNode;
    restoreNode.id = "m_restoreSlot";
    restoreNode.key = "m_restoreSlot";
    restoreNode.title = "Restore previous hotbar slot";
    restoreNode.category = "combat";
    restoreNode.type = ConfigControlTypeV2::Toggle;
    schema.node(std::move(restoreNode));

    ConfigNodeV2 manualNode;
    manualNode.id = "m_respectManualSlotChanges";
    manualNode.key = "m_respectManualSlotChanges";
    manualNode.title = "Respect manual hotbar changes";
    manualNode.category = "combat";
    manualNode.type = ConfigControlTypeV2::Toggle;
    schema.node(std::move(manualNode));

    ConfigNodeV2 keybindNode;
    keybindNode.id = "keybind";
    keybindNode.key = "keybind";
    keybindNode.title = "Keybind";
    keybindNode.category = "combat";
    keybindNode.type = ConfigControlTypeV2::Keybind;
    schema.node(std::move(keybindNode));

    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

bool SwordSwapperModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "m_restoreSlot" || key == "m_respectManualSlotChanges") {
        const bool flag = value == "true" || value == "1" || value == "on";
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (key == "m_restoreSlot") m_restoreSlot = flag;
        else m_respectManualSlotChanges = flag;
        return true;
    }

    try {
        std::size_t parsedLength = 0;
        const std::string text(value);
        const float number = std::stof(text, &parsedLength);
        while (parsedLength < text.size() &&
               std::isspace(static_cast<unsigned char>(text[parsedLength]))) ++parsedLength;
        if (parsedLength != text.size() || !std::isfinite(number)) return false;

        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (key == "m_attackRange") m_attackRange = std::clamp(number, 2.0f, 6.0f);
        else if (key == "m_groundDistance") m_groundDistance = std::clamp(number, 0.0f, 3.0f);
        else return false;
        return true;
    } catch (...) {
        return false;
    }
}
