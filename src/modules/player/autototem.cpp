#include "autototem.hpp"

#include "AutoTotemPolicy.hpp"
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
    m_nextInventoryScan = {};
    m_cachedTotemSlot = -1;
}

void AutoTotemModule::onDisable() {
    std::lock_guard lock(m_stateMutex);
    m_pendingRefill = false;
    m_lastOffhandHadTotem = false;
    m_refillAfter = {};
    m_nextInventoryScan = {};
    m_cachedTotemSlot = -1;
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
        m_nextInventoryScan = {};
        m_cachedTotemSlot = -1;
        return;
    }

    // Do not remove, overwrite, or attempt to move an item from a non-empty offhand.
    if (!offhandEmpty) {
        m_lastOffhandHadTotem = false;
        m_pendingRefill = false;
        m_refillAfter = {};
        m_nextInventoryScan = {};
        m_cachedTotemSlot = -1;
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
    if (m_combatOnly && !inCombat) {
        auto* aimBase = ModuleRegistry::get().find("bedrocktools.Aim Assist");
        if (aimBase && aimBase->enabled) {
            auto* aim = static_cast<AimAssistModule*>(aimBase);
            void* target = nullptr;
            bedrocktools::sdk::AABB targetBounds{};
            float targetRange = 0.0f;
            if (aim->currentTargetSnapshot(target, targetBounds, targetRange)) inCombat = true;
        }
    }

    const bool containerOpen = m_containerOpen.load(std::memory_order_acquire);
    const bool delayElapsed =
        m_refillAfter.time_since_epoch().count() == 0 || now >= m_refillAfter;
    const bedaim::autototem::Inputs precheck{
        enabled, m_pendingRefill, offhandHasTotem, delayElapsed,
        m_combatOnly, inCombat, containerOpen, false, offhandEmpty
    };

    // Skip inventory slot scans when delay/container/combat gates block the refill.
    // If the inventory has no totem, do not walk all 36+ slots again every game
    // tick. Cache the last miss briefly and expose the retry interval in settings.
    auto decision = bedaim::autototem::decide(precheck);
    int totemSlot = -1;
    if (decision == bedaim::autototem::Decision::WaitForItem) {
        if (m_cachedTotemSlot >= 0) {
            totemSlot = m_cachedTotemSlot;
        } else if (m_nextInventoryScan.time_since_epoch().count() == 0 ||
                   now >= m_nextInventoryScan) {
            totemSlot = inventory.findInventorySlot(player, "totem");
            if (totemSlot >= 0) {
                m_cachedTotemSlot = totemSlot;
                m_nextInventoryScan = {};
            } else {
                m_nextInventoryScan = now + std::chrono::milliseconds(
                    static_cast<int>(bedaim::autototem::normalizeScanRetryMs(m_scanRetryIntervalMs)));
            }
        }

        const bedaim::autototem::Inputs withCandidate{
            enabled, m_pendingRefill, offhandHasTotem, delayElapsed,
            m_combatOnly, inCombat, containerOpen, totemSlot >= 0, offhandEmpty
        };
        decision = bedaim::autototem::decide(withCandidate);
    }

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
                m_nextInventoryScan = {};
                m_cachedTotemSlot = -1;
            } else {
                // Fail closed and retry at a modest cadence if this build lacks a
                // compatible native route or a local/server-visible transfer fails.
                if (!g_loggedAutoTotemTransferFailure.exchange(true, std::memory_order_acq_rel))
                    autoTotemLogger().warn("Auto Totem first transfer failed safely: {}. No unverified setter will be called.", transferResultName(result));
                m_cachedTotemSlot = -1;
                m_refillAfter = now + std::chrono::milliseconds(
                    static_cast<int>(bedaim::autototem::normalizeScanRetryMs(m_scanRetryIntervalMs)));
                m_nextInventoryScan = m_refillAfter;
            }
            break;
        }
        case bedaim::autototem::Decision::Idle:
            m_pendingRefill = false;
            m_refillAfter = {};
            m_nextInventoryScan = {};
            m_cachedTotemSlot = -1;
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
    m_scanRetryIntervalMs = bedaim::autototem::normalizeScanRetryMs(
        json.value("m_scanRetryIntervalMs", m_scanRetryIntervalMs));
    m_combatOnly = json.value("m_combatOnly", m_combatOnly);
    m_keepTotemEquipped = json.value("m_keepTotemEquipped", m_keepTotemEquipped);
}

void AutoTotemModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard lock(m_stateMutex);
    json["m_delayMs"] = bedaim::autototem::normalizeDelayMs(m_delayMs);
    json["m_scanRetryIntervalMs"] = bedaim::autototem::normalizeScanRetryMs(m_scanRetryIntervalMs);
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
    delay.description = "Time to wait after a totem is consumed before refilling.";
    schema.node(std::move(delay));
    auto scanRetry = node("m_scanRetryIntervalMs", "Inventory scan retry (ms)", "behavior",
                          ConfigControlTypeV2::SliderFloat);
    scanRetry.minValue = "100";
    scanRetry.maxValue = "2000";
    scanRetry.step = "50";
    scanRetry.description = "How often to rescan inventory when no totem is found. Higher values reduce lag with an empty inventory.";
    schema.node(std::move(scanRetry));
    schema.node(node("m_keepTotemEquipped", "Keep Totem Equipped", "behavior",
                     ConfigControlTypeV2::Toggle));
    schema.node(node("m_combatOnly", "Combat Only", "safety",
                     ConfigControlTypeV2::Toggle));
    schema.node(node("keybind", "Keybind", "behavior", ConfigControlTypeV2::Keybind));
    setConfigSchemaJson(moduleId, schema.toJson());
}
