#include "autototem.hpp"

#include "AutoTotemPolicy.hpp"
#include "aimassist.hpp"
#include "core/GameHooks.hpp"
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
    : Module("BedrockGuard",
             "BedrockGuard: offhand protection and totem refill.", "Auto Totem") {
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
    if (m_uiSequencePhase != UiSequencePhase::Idle) return;
    m_pendingRefill = false;
    m_lastOffhandHadTotem = false;
    m_refillAfter = {};
    m_nextInventoryScan = {};
    m_nextOffhandCheck = {};
    m_uiPhaseDeadline = {};
    m_cachedTotemSlot = -1;
    m_uiTransferSucceeded = false;
}

void AutoTotemModule::onDisable() {
    std::lock_guard lock(m_stateMutex);
    if (m_uiSequencePhase != UiSequencePhase::Idle) {
        // Finish closing an inventory screen opened by this module from the
        // next game tick. Do not call native UI methods from the menu thread.
        m_pendingRefill = false;
        m_uiTransferSucceeded = false;
        return;
    }
    m_pendingRefill = false;
    m_lastOffhandHadTotem = false;
    m_refillAfter = {};
    m_nextInventoryScan = {};
    m_nextOffhandCheck = {};
    m_uiPhaseDeadline = {};
    m_cachedTotemSlot = -1;
}

void AutoTotemModule::processPlayerTick(bedrocktools::sdk::Player* player) {
    if (!player) return;

    auto& inventory = bedrocktools::core::InventoryAccess::get();
    const auto checkTime = std::chrono::steady_clock::now();

    // One-shot request from the floating button; native inventory work stays on
    // the LocalPlayer tick thread rather than the launcher UI callback.
    if (m_manualRequest.exchange(false, std::memory_order_acq_rel)) {
        const std::string currentOffhand = inventory.offhandIdentifier(player);
        if (containsTotem(currentOffhand)) {
            autoTotemLogger().info("Manual equip ignored: a totem is already equipped.");
            return;
        }
        if (!currentOffhand.empty()) {
            autoTotemLogger().warn("Manual equip refused: offhand is occupied.");
            return;
        }
        const int totemSlot = inventory.findInventorySlot(player, "totem");
        if (totemSlot < 0) {
            autoTotemLogger().warn("Manual equip failed: no totem was found in inventory.");
            return;
        }
        // First validate the actual inventory-to-offhand transaction without
        // opening UI. Opening inventory first left the user stuck on builds where
        // the native close helper is not exported, while hiding the real transfer
        // failure. Keep this one-shot operation on the player tick thread.
        const auto transfer = inventory.moveInventoryItemToOffhand(player, totemSlot);
        const bool equipped =
            transfer == bedrocktools::core::OffhandTransferResult::Success ||
            transfer == bedrocktools::core::OffhandTransferResult::AlreadyEquipped;
        {
            std::lock_guard lock(m_stateMutex);
            m_uiSequencePhase = UiSequencePhase::Idle;
            m_uiPhaseDeadline = {};
            m_manualSequenceActive = false;
            m_uiTransferSucceeded = equipped;
            m_cachedTotemSlot = -1;
            m_pendingRefill = false;
            if (equipped) {
                m_lastOffhandHadTotem = true;
                m_refillAfter = {};
                m_nextInventoryScan = {};
                m_nextOffhandCheck = {};
            } else {
                m_nextInventoryScan = checkTime + std::chrono::milliseconds(
                    static_cast<int>(bedaim::autototem::normalizeScanRetryMs(m_scanRetryIntervalMs)));
            }
        }
        if (equipped) {
            autoTotemLogger().info("Manual AutoTotem transfer result: {}.", transferResultName(transfer));
        } else {
            autoTotemLogger().error(
                "Manual AutoTotem transfer failed: {}. Inventory UI was not opened; this result identifies the native API/transaction blocker.",
                transferResultName(transfer));
        }
        return;
    }
    bool readOffhand = false;
    {
        std::lock_guard lock(m_stateMutex);
        if (!enabled && m_uiSequencePhase == UiSequencePhase::Idle) return;
        if (m_uiSequencePhase == UiSequencePhase::Idle) {
            if (m_nextOffhandCheck.time_since_epoch().count() != 0 &&
                checkTime < m_nextOffhandCheck) return;
            m_nextOffhandCheck = checkTime + std::chrono::milliseconds(
                static_cast<int>(bedaim::autototem::normalizeOffhandPollMs(m_offhandPollIntervalMs)));
            readOffhand = true;
        } else if (m_uiSequencePhase == UiSequencePhase::WaitBeforeTransfer &&
                   (!enabled || checkTime >= m_uiPhaseDeadline)) {
            // Only query the native offhand stack when a UI sequence reaches a
            // decision point. Waiting between open/close operations doesn't need
            // another expensive ItemStack description lookup every game tick.
            readOffhand = true;
        }
    }

    bool offhandHasTotem = false;
    bool offhandEmpty = true;
    if (readOffhand) {
        const std::string offhandId = inventory.offhandIdentifier(player);
        offhandHasTotem = containsTotem(offhandId);
        offhandEmpty = offhandId.empty();
    }
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(m_stateMutex);
    if (!enabled && m_uiSequencePhase == UiSequencePhase::Idle) return;

    auto completeUiSequence = [&]() {
        m_uiSequencePhase = UiSequencePhase::Idle;
        m_uiPhaseDeadline = {};
        m_nextOffhandCheck = {};
        m_manualSequenceActive = false;
        if (m_uiTransferSucceeded) {
            m_pendingRefill = false;
            m_lastOffhandHadTotem = true;
            m_refillAfter = {};
            m_nextInventoryScan = {};
            m_cachedTotemSlot = -1;
        } else if (enabled && m_pendingRefill) {
            if (m_nextInventoryScan.time_since_epoch().count() == 0) {
                m_nextInventoryScan = now + std::chrono::milliseconds(
                    static_cast<int>(bedaim::autototem::normalizeScanRetryMs(m_scanRetryIntervalMs)));
            }
            m_refillAfter = m_nextInventoryScan;
        }
        m_uiTransferSucceeded = false;
    };

    // Confirm that the inventory screen actually appeared before transferring.
    // Some BedrockTools builds deliver the screen event on a later tick, while
    // unsupported open wrappers can return without opening any visible UI.
    if (m_uiSequencePhase == UiSequencePhase::WaitForOpen) {
        if (!enabled && !m_manualSequenceActive) {
            if (m_containerOpen.load(std::memory_order_acquire)) {
                m_uiSequencePhase = UiSequencePhase::WaitBeforeClose;
                m_uiPhaseDeadline = now + std::chrono::milliseconds(
                    static_cast<int>(bedaim::autototem::normalizeUiStepDelayMs(m_uiStepDelayMs)));
            } else {
                completeUiSequence();
            }
            return;
        }
        if (m_containerOpen.load(std::memory_order_acquire)) {
            m_uiSequencePhase = UiSequencePhase::WaitBeforeTransfer;
            m_uiPhaseDeadline = now + std::chrono::milliseconds(
                static_cast<int>(bedaim::autototem::normalizeUiStepDelayMs(m_uiStepDelayMs)));
            m_nextOffhandCheck = {};
            return;
        }
        if (now < m_uiPhaseDeadline) return;

        if (m_manualSequenceActive) {
            // Minecraft can open the ordinary inventory from KEYCODE_E without
            // emitting the Container ScreenStateEvent in some launcher builds.
            // Continue the manual sequence after a bounded wait. The existing
            // transfer step still revalidates the offhand and source slot.
            autoTotemLogger().warn(
                "Manual inventory opened by E but no Container screen event arrived; continuing with a verified offhand transfer.");
            m_uiSequencePhase = UiSequencePhase::WaitBeforeTransfer;
            m_uiPhaseDeadline = now + std::chrono::milliseconds(
                static_cast<int>(bedaim::autototem::normalizeUiStepDelayMs(m_uiStepDelayMs)));
            m_nextOffhandCheck = {};
            return;
        }

        autoTotemLogger().warn(
            "Auto Totem inventory did not open within 600 ms; aborting automatic UI sequence.");
        m_uiSequencePhase = UiSequencePhase::Idle;
        m_uiPhaseDeadline = {};
        m_nextOffhandCheck = {};
        m_manualSequenceActive = false;
        m_uiTransferSucceeded = false;
        m_pendingRefill = false;
        m_cachedTotemSlot = -1;
        return;
    }

    // A request is executed on the player tick thread, with bounded delay between
    // the actual LocalPlayer open, native inventory transaction and close request.
    if (m_uiSequencePhase == UiSequencePhase::WaitBeforeTransfer) {
        if (!enabled && !m_manualSequenceActive) {
            m_uiTransferSucceeded = false;
            m_uiSequencePhase = UiSequencePhase::WaitBeforeClose;
            m_uiPhaseDeadline = now + std::chrono::milliseconds(
                static_cast<int>(bedaim::autototem::normalizeUiStepDelayMs(m_uiStepDelayMs)));
            return;
        }
        if (offhandHasTotem) {
            m_uiTransferSucceeded = true;
            m_pendingRefill = false;
            m_uiSequencePhase = UiSequencePhase::WaitBeforeClose;
            m_uiPhaseDeadline = now + std::chrono::milliseconds(
                static_cast<int>(bedaim::autototem::normalizeUiStepDelayMs(m_uiStepDelayMs)));
            return;
        }
        if (now < m_uiPhaseDeadline) return;

        int totemSlot = m_cachedTotemSlot;
        if (totemSlot < 0) totemSlot = inventory.findInventorySlot(player, "totem");
        const auto result = totemSlot >= 0
            ? inventory.moveInventoryItemToOffhand(player, totemSlot)
            : bedrocktools::core::OffhandTransferResult::ItemUnavailable;
        const bool success = result == bedrocktools::core::OffhandTransferResult::Success ||
            result == bedrocktools::core::OffhandTransferResult::AlreadyEquipped;
        m_uiTransferSucceeded = success;
        if (success) {
            if (!g_loggedAutoTotemTransferSuccess.exchange(true, std::memory_order_acq_rel))
                autoTotemLogger().info("Auto Totem UI sequence transfer result: {}.", transferResultName(result));
            m_pendingRefill = false;
            m_lastOffhandHadTotem = true;
            m_refillAfter = {};
            m_nextInventoryScan = {};
            m_cachedTotemSlot = -1;
        } else {
            if (!g_loggedAutoTotemTransferFailure.exchange(true, std::memory_order_acq_rel))
                autoTotemLogger().warn("Auto Totem UI sequence transfer failed safely: {}.", transferResultName(result));
            m_cachedTotemSlot = -1;
            m_nextInventoryScan = now + std::chrono::milliseconds(
                static_cast<int>(bedaim::autototem::normalizeScanRetryMs(m_scanRetryIntervalMs)));
            m_refillAfter = m_nextInventoryScan;
        }
        m_uiSequencePhase = UiSequencePhase::WaitBeforeClose;
        m_uiPhaseDeadline = now + std::chrono::milliseconds(
            static_cast<int>(bedaim::autototem::normalizeUiStepDelayMs(m_uiStepDelayMs)));
        return;
    }

    if (m_uiSequencePhase == UiSequencePhase::WaitBeforeClose) {
        if (now < m_uiPhaseDeadline) return;
        if (!bedrocktools::core::gamehooks::closePlayerInventoryScreen(player)) {
            autoTotemLogger().warn("Auto Totem could not request inventory close; resetting state instead of retrying an unverified UI call.");
            m_uiSequencePhase = UiSequencePhase::Idle;
            m_uiPhaseDeadline = {};
            m_manualSequenceActive = false;
            m_pendingRefill = false;
            m_uiTransferSucceeded = false;
            m_cachedTotemSlot = -1;
            return;
        }
        m_uiSequencePhase = UiSequencePhase::WaitForClose;
        m_uiPhaseDeadline = now + std::chrono::milliseconds(600);
        return;
    }

    if (m_uiSequencePhase == UiSequencePhase::WaitForClose) {
        if (!m_containerOpen.load(std::memory_order_acquire)) {
            completeUiSequence();
            return;
        }
        if (now < m_uiPhaseDeadline) return;
        // closeContainerScreen queues a close on the LocalPlayer. Retry at a
        // moderate cadence if the launcher did not deliver the close event yet.
        if (!bedrocktools::core::gamehooks::closePlayerInventoryScreen(player)) {
            autoTotemLogger().warn("Auto Totem could not confirm inventory close; leaving remaining UI control to the player.");
            m_uiSequencePhase = UiSequencePhase::Idle;
            m_uiPhaseDeadline = {};
            m_uiTransferSucceeded = false;
            m_manualSequenceActive = false;
            m_pendingRefill = false;
            m_cachedTotemSlot = -1;
            return;
        }
        m_uiPhaseDeadline = now + std::chrono::milliseconds(600);
        return;
    }

    if (!readOffhand) return;

    if (offhandHasTotem) {
        m_lastOffhandHadTotem = true;
        m_pendingRefill = false;
        m_refillAfter = {};
        m_nextInventoryScan = {};
        m_cachedTotemSlot = -1;
        return;
    }
    if (!enabled) return;

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
        m_pendingRefill = true;
        m_refillAfter = now + std::chrono::milliseconds(
            static_cast<int>(bedaim::autototem::normalizeDelayMs(m_delayMs)));
        m_lastOffhandHadTotem = false;
    } else if (m_keepTotemEquipped && !m_pendingRefill) {
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
            // Do not open the visible inventory until both transfer and close APIs
            // have been proven on this runtime. The direct native transaction either
            // equips and verifies the totem or returns a specific failure reason;
            // this avoids leaving the player's inventory open after an API miss.
            const auto result = inventory.moveInventoryItemToOffhand(player, totemSlot);
            if (result == bedrocktools::core::OffhandTransferResult::Success ||
                result == bedrocktools::core::OffhandTransferResult::AlreadyEquipped) {
                if (!g_loggedAutoTotemTransferSuccess.exchange(true, std::memory_order_acq_rel))
                    autoTotemLogger().info("Auto Totem native transfer result: {}.", transferResultName(result));
                m_pendingRefill = false;
                m_lastOffhandHadTotem = true;
                m_refillAfter = {};
                m_nextInventoryScan = {};
                m_cachedTotemSlot = -1;
            } else {
                if (!g_loggedAutoTotemTransferFailure.exchange(true, std::memory_order_acq_rel))
                    autoTotemLogger().warn("Auto Totem native transfer failed safely: {}.", transferResultName(result));
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
    m_offhandPollIntervalMs = bedaim::autototem::normalizeOffhandPollMs(
        json.value("m_offhandPollIntervalMs", m_offhandPollIntervalMs));
    m_uiStepDelayMs = bedaim::autototem::normalizeUiStepDelayMs(
        json.value("m_uiStepDelayMs", m_uiStepDelayMs));
    m_useInventoryUiSequence = json.value("m_useInventoryUiSequence", m_useInventoryUiSequence);
    m_combatOnly = json.value("m_combatOnly", m_combatOnly);
    m_keepTotemEquipped = json.value("m_keepTotemEquipped", m_keepTotemEquipped);
}

void AutoTotemModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard lock(m_stateMutex);
    json["m_delayMs"] = bedaim::autototem::normalizeDelayMs(m_delayMs);
    json["m_scanRetryIntervalMs"] = bedaim::autototem::normalizeScanRetryMs(m_scanRetryIntervalMs);
    json["m_offhandPollIntervalMs"] = bedaim::autototem::normalizeOffhandPollMs(m_offhandPollIntervalMs);
    json["m_uiStepDelayMs"] = bedaim::autototem::normalizeUiStepDelayMs(m_uiStepDelayMs);
    json["m_useInventoryUiSequence"] = m_useInventoryUiSequence;
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
    scanRetry.minValue = "400";
    scanRetry.maxValue = "2000";
    scanRetry.step = "50";
    scanRetry.description = "How often to rescan inventory when no totem is found. Minimum 400 ms avoids repeated full-inventory ItemStack lookups on slower phones.";
    schema.node(std::move(scanRetry));
    auto offhandPoll = node("m_offhandPollIntervalMs", "Offhand check interval (ms)", "behavior",
                            ConfigControlTypeV2::SliderFloat);
    offhandPoll.minValue = "150";
    offhandPoll.maxValue = "500";
    offhandPoll.step = "10";
    offhandPoll.description = "How often AutoTotem checks the offhand. Minimum 150 ms reduces native ItemStack lookups while keeping refill detection responsive.";
    schema.node(std::move(offhandPoll));
    // The inventory UI automation controls remain hidden until the matching
    // native open/close path is validated on the target Bedrock runtime. Persisted
    // values are still accepted for compatibility with older configuration files.
    schema.node(node("m_keepTotemEquipped", "Keep Totem Equipped", "behavior",
                     ConfigControlTypeV2::Toggle));
    ButtonBuilder manualButton("bedaim.autototem.manual_equip", "AutoTotem Equip");
    manualButton.moduleId(moduleId)
        .modId("BedrockTools")
        .label("EQUIP TOTEM")
        .behavior(ButtonBehavior::Click)
        .defaultVisible(true)
        .stylePreset(ButtonStylePreset::Keycap)
        .onEvent([this](std::string_view, ButtonEvent event, float) {
            if (event == ButtonEvent::Click)
                m_manualRequest.store(true, std::memory_order_release);
        }).registerButton();
    schema.node(node("m_combatOnly", "Combat Only", "safety",
                     ConfigControlTypeV2::Toggle));
    schema.node(node("keybind", "Keybind", "behavior", ConfigControlTypeV2::Keybind));
    setConfigSchemaJson(moduleId, schema.toJson());
}
