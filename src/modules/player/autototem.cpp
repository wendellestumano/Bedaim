#include "autototem.hpp"

#include "AutoTotemPolicy.hpp"
#include "aimassist.hpp"
#include "core/InventoryAccess.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/events/ScreenStateEvent.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace {
bool containsTotem(std::string_view identifier) {
    return identifier.find("totem") != std::string_view::npos;
}
}

AutoTotemModule::AutoTotemModule()
    : Module("Auto Totem",
             "Refills an empty offhand with a totem using native inventory transactions.") {
    keybind = 0;
}

void AutoTotemModule::onInit() {
    bedrocktools::core::InventoryAccess::get().initialize();

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { processPlayerTick(event.player); });

    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>(
        [this](auto& event) {
            if (event.screen != bedrocktools::events::ScreenKind::Container) return;
            m_containerOpen.store(event.phase == bedrocktools::events::ScreenPhase::Opened,
                                  std::memory_order_release);
        });
}

void AutoTotemModule::onEnable() {
    std::lock_guard lock(m_stateMutex);
    m_pendingRefill = false;
    m_lastOffhandHadTotem = false;
    m_refillAfter = {};
}

void AutoTotemModule::onDisable() {
    std::lock_guard lock(m_stateMutex);
    m_pendingRefill = false;
    m_lastOffhandHadTotem = false;
    m_refillAfter = {};
}

void AutoTotemModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    if (!enabled || !player) return;

    auto& inventory = bedrocktools::core::InventoryAccess::get();
    const std::string offhandId = inventory.offhandIdentifier(player);
    const bool offhandHasTotem = containsTotem(offhandId);
    const bool offhandEmpty = offhandId.empty();
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(m_stateMutex);
    if (!enabled) return;

    if (offhandHasTotem) {
        m_lastOffhandHadTotem = true;
        m_pendingRefill = false;
        m_refillAfter = {};
        return;
    }

    // Do not remove, overwrite, or attempt to move an item from a non-empty offhand.
    if (!offhandEmpty) {
        m_lastOffhandHadTotem = false;
        m_pendingRefill = false;
        m_refillAfter = {};
        return;
    }

    if (m_lastOffhandHadTotem) {
        // This transition is the consumption signal. Start the user-configured
        // delay once, not on every tick while the offhand remains empty.
        m_pendingRefill = true;
        m_refillAfter = now + std::chrono::milliseconds(
            static_cast<int>(bedaim::autototem::normalizeDelayMs(m_delayMs)));
        m_lastOffhandHadTotem = false;
    } else if (m_keepTotemEquipped && !m_pendingRefill) {
        // When enabled with an empty offhand, Keep Totem Equipped can fill it
        // immediately; the post-consumption delay above remains configurable.
        m_pendingRefill = true;
        m_refillAfter = now;
    }

    if (!m_pendingRefill) return;

    bool inCombat = player->hurtTime() > 0;
    auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
    if (aimBase && aimBase->enabled) {
        auto* aim = static_cast<AimAssistModule*>(aimBase);
        void* target = nullptr;
        bedrocktools::sdk::AABB targetBounds{};
        float targetRange = 0.0f;
        if (aim->currentTargetSnapshot(target, targetBounds, targetRange)) {
            inCombat = true;
        }
    }

    const bool containerOpen = m_containerOpen.load(std::memory_order_acquire);
    const int totemSlot = inventory.findInventorySlot(player, "totem");
    const bool delayElapsed =
        m_refillAfter.time_since_epoch().count() == 0 || now >= m_refillAfter;
    const bedaim::autototem::Inputs input{
        enabled,
        m_pendingRefill,
        offhandHasTotem,
        delayElapsed,
        m_combatOnly,
        inCombat,
        containerOpen,
        totemSlot >= 0,
        offhandEmpty
    };

    switch (bedaim::autototem::decide(input)) {
        case bedaim::autototem::Decision::Equip: {
            const auto result = inventory.moveInventoryItemToOffhand(player, totemSlot);
            if (result == bedrocktools::core::OffhandTransferResult::Success ||
                result == bedrocktools::core::OffhandTransferResult::AlreadyEquipped) {
                m_pendingRefill = false;
                m_lastOffhandHadTotem = true;
                m_refillAfter = {};
            } else {
                // Fail closed and retry at a modest cadence if this build lacks a
                // compatible native route or a local/server-visible transfer fails.
                m_refillAfter = now + std::chrono::milliseconds(100);
            }
            break;
        }
        case bedaim::autototem::Decision::Idle:
            m_pendingRefill = false;
            m_refillAfter = {};
            break;
        case bedaim::autototem::Decision::WaitForDelay:
        case bedaim::autototem::Decision::WaitForCombat:
        case bedaim::autototem::Decision::WaitForContainer:
        case bedaim::autototem::Decision::WaitForItem:
        case bedaim::autototem::Decision::WaitForOffhandClear:
            break;
    }
}

void AutoTotemModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard lock(m_stateMutex);
    m_delayMs = bedaim::autototem::normalizeDelayMs(
        json.value("m_delayMs", m_delayMs));
    m_combatOnly = json.value("m_combatOnly", m_combatOnly);
    m_keepTotemEquipped = json.value("m_keepTotemEquipped", m_keepTotemEquipped);
}

void AutoTotemModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard lock(m_stateMutex);
    json["m_delayMs"] = bedaim::autototem::normalizeDelayMs(m_delayMs);
    json["m_combatOnly"] = m_combatOnly;
    json["m_keepTotemEquipped"] = m_keepTotemEquipped;
}

void AutoTotemModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("behavior")
        .category("behavior", "Behavior", "Automatic offhand refill")
        .category("safety", "Safety", "Conditions that must be met before a refill");

    auto node = [](const char* key, const char* title, const char* category,
                   ConfigControlTypeV2 type) {
        ConfigNodeV2 value;
        value.id = key;
        value.key = key;
        value.title = title;
        value.category = category;
        value.type = type;
        return value;
    };

    auto delay = node("m_delayMs", "Refill Delay (ms)", "behavior",
                      ConfigControlTypeV2::SliderFloat);
    delay.minValue = "0";
    delay.maxValue = "1000";
    delay.step = "10";
    schema.node(std::move(delay));
    schema.node(node("m_keepTotemEquipped", "Keep Totem Equipped", "behavior",
                     ConfigControlTypeV2::Toggle));
    schema.node(node("m_combatOnly", "Combat Only", "safety",
                     ConfigControlTypeV2::Toggle));
    schema.node(node("keybind", "Keybind", "behavior", ConfigControlTypeV2::Keybind));
    setConfigSchemaJson(moduleId, schema.toJson());
}
