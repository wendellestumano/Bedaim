#include "autototem.hpp"

#include "AutoTotemPolicy.hpp"
#include "../../core/Diagnostics.hpp"
#include "aimassist.hpp"
#include "core/InventoryAccess.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/events/ScreenStateEvent.hpp>
#include <pl/ModMenu.hpp>
#include <pl/Logger.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace {
pl::log::Logger& autoTotemLogger() {
    static auto& logger = pl::log::Logger::getOrCreate("Bedaim-AutoTotem");
    return logger;
}

std::atomic_bool g_loggedAutoTotemTransferSuccess{false};
std::atomic_bool g_loggedAutoTotemTransferFailure{false};

const char* transferResultName(bedrocktools::core::OffhandTransferResult result) noexcept {
    using bedrocktools::core::OffhandTransferResult;
    switch (result) {
        case OffhandTransferResult::Success: return "success";
        case OffhandTransferResult::AlreadyEquipped: return "already equipped";
        case OffhandTransferResult::InvalidPlayer: return "invalid player";
        case OffhandTransferResult::InvalidSlot: return "invalid inventory slot";
        case OffhandTransferResult::ItemUnavailable: return "totem unavailable in source slot";
        case OffhandTransferResult::OffhandOccupied: return "offhand occupied";
        case OffhandTransferResult::RuntimeApiUnavailable: return "verified native inventory API unavailable";
        case OffhandTransferResult::InvalidNativeState: return "native inventory state failed validation";
        case OffhandTransferResult::VerificationFailed: return "offhand/source verification failed";
    }
    return "unknown transfer result";
}

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
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
        "AutoTotem", "lifecycle", "initialized", "inventory and screen-state hooks subscribed");

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
    if (!enabled || !player) {
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Warning,
            "AutoTotem", "tick_gate", !enabled ? "disabled" : "missing_player");
        return;
    }
    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
        "AutoTotem", "tick_gate", "ready");

    auto& inventory = bedrocktools::core::InventoryAccess::get();
    const std::string offhandId = inventory.offhandIdentifier(player);
    const bool offhandHasTotem = containsTotem(offhandId);
    const bool offhandEmpty = offhandId.empty();
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(m_stateMutex);
    if (!enabled) return;

    bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Debug,
        "AutoTotem", "offhand_state", offhandHasTotem ? "totem_equipped" :
            offhandEmpty ? "empty" : "occupied_by_other_item");

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

    const auto decision = bedaim::autototem::decide(input);
    const char* decisionState = "unknown";
    switch (decision) {
        case bedaim::autototem::Decision::Idle: decisionState = "idle"; break;
        case bedaim::autototem::Decision::WaitForDelay: decisionState = "waiting_for_delay"; break;
        case bedaim::autototem::Decision::WaitForCombat: decisionState = "waiting_for_combat"; break;
        case bedaim::autototem::Decision::WaitForContainer: decisionState = "waiting_for_container"; break;
        case bedaim::autototem::Decision::WaitForItem: decisionState = "waiting_for_totem_item"; break;
        case bedaim::autototem::Decision::WaitForOffhandClear: decisionState = "offhand_not_empty"; break;
        case bedaim::autototem::Decision::Equip: decisionState = "attempting_transfer"; break;
    }
    bedrocktools::diagnostics::probe(
        decision == bedaim::autototem::Decision::Equip ? bedrocktools::diagnostics::Level::Info :
            bedrocktools::diagnostics::Level::Debug,
        "AutoTotem", "refill_decision", decisionState);
    switch (decision) {
        case bedaim::autototem::Decision::Equip: {
            const auto result = inventory.moveInventoryItemToOffhand(player, totemSlot);
            if (result == bedrocktools::core::OffhandTransferResult::Success ||
                result == bedrocktools::core::OffhandTransferResult::AlreadyEquipped) {
                if (!g_loggedAutoTotemTransferSuccess.exchange(true, std::memory_order_acq_rel))
                    autoTotemLogger().info("Auto Totem first transfer result: {}.", transferResultName(result));
                m_pendingRefill = false;
                m_lastOffhandHadTotem = true;
                m_refillAfter = {};
            } else {
                // Fail closed and retry at a modest cadence if this build lacks a
                // compatible native route or a local/server-visible transfer fails.
                if (!g_loggedAutoTotemTransferFailure.exchange(true, std::memory_order_acq_rel))
                    autoTotemLogger().warn("Auto Totem first transfer failed safely: {}. No unverified setter will be called.", transferResultName(result));
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

bool AutoTotemModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "m_combatOnly" || key == "m_keepTotemEquipped") {
        const bool enabledValue = value == "true" || value == "1" || value == "on" || value == "yes";
        const bool disabledValue = value == "false" || value == "0" || value == "off" || value == "no";
        if (!enabledValue && !disabledValue) return false;
        std::lock_guard lock(m_stateMutex);
        if (key == "m_combatOnly") m_combatOnly = enabledValue;
        else m_keepTotemEquipped = enabledValue;
        bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
            "AutoTotem", key, enabledValue ? "enabled" : "disabled");
        return true;
    }

    if (key == "m_delayMs") {
        try {
            const std::string raw(value);
            std::size_t parsed = 0;
            const float number = std::stof(raw, &parsed);
            while (parsed < raw.size() && std::isspace(static_cast<unsigned char>(raw[parsed]))) ++parsed;
            if (parsed != raw.size() || !std::isfinite(number)) return false;
            std::lock_guard lock(m_stateMutex);
            m_delayMs = bedaim::autototem::normalizeDelayMs(number);
            bedrocktools::diagnostics::probe(bedrocktools::diagnostics::Level::Info,
                "AutoTotem", "refill_delay_ms", std::to_string(static_cast<int>(m_delayMs)));
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
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
