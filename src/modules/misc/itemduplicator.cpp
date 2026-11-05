#include "itemduplicator.hpp"

#include "../../config/AtomicConfigWriter.hpp"
#include "../../config/ConfigManager.hpp"
#include "../../core/InventoryAccess.hpp"
#include "../../core/Diagnostics.hpp"
#include <bedrocktools/events/ContainerSlotSelectedEvent.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using json = nlohmann::json;

bool parseBool(std::string_view value, bool& result) {
    if (value == "true" || value == "1" || value == "on") { result = true; return true; }
    if (value == "false" || value == "0" || value == "off") { result = false; return true; }
    return false;
}

bool parseNumber(std::string_view value, double& result) {
    try {
        std::size_t used = 0;
        const std::string text(value);
        const double parsed = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(parsed)) return false;
        result = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

int configInteger(const json& config, const char* key, int fallback, int minValue, int maxValue, int step) {
    const auto it = config.find(key);
    if (it == config.end() || !it->is_number()) return fallback;
    const double raw = it->get<double>();
    if (!std::isfinite(raw)) return fallback;
    const double boundedValue = std::clamp(raw, static_cast<double>(minValue), static_cast<double>(maxValue));
    const int bounded = static_cast<int>(std::lround(boundedValue));
    const int snapped = minValue + static_cast<int>(std::lround(
        static_cast<double>(bounded - minValue) / static_cast<double>(step))) * step;
    return std::clamp(snapped, minValue, maxValue);
}

bool configBool(const json& config, const char* key, bool fallback) {
    const auto it = config.find(key);
    return it != config.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

std::string observerLogPath() {
    const std::filesystem::path configPath(bedrocktools::config::ConfigManager::get().getConfigPath());
    std::filesystem::path parent = configPath.parent_path();
    if (parent.empty()) parent = "/sdcard/Download/Bedaim";
    return (parent / "InventoryObserver.jsonl").string();
}
}

ItemDuplicatorModule::ItemDuplicatorModule()
    : Module("Item Duplicator",
             "Inventory observer plus a Creative-only Main Hand copy test. The test does not bypass server authority or replay packets.") {
    keybind = 0;
}

ItemDuplicatorModule::~ItemDuplicatorModule() {
    unsubscribeEvents();
    flushLog();
}

void ItemDuplicatorModule::onInit() {}

void ItemDuplicatorModule::onEnable() {
    const auto now = std::chrono::steady_clock::now();
    const auto session = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    {
        std::lock_guard<std::mutex> lock(m_recordsMutex);
        m_records.clear();
        m_previous = {};
        m_hasPrevious = false;
        m_sessionId = session;
        m_logDirty = true;
        ++m_logGeneration;
    }
    m_captureRequested.store(true, std::memory_order_release);
    m_enabledAt = now;
    m_callbacksActive.store(true, std::memory_order_release);
    m_nextCapture = {};
    m_nextFlush = now + std::chrono::seconds(5);

    auto& events = bedrocktools::events::bus();
    if (m_tickSubscription == 0) {
        m_tickSubscription = events.subscribe<bedrocktools::events::LocalPlayerTickEvent>(
            [this](bedrocktools::events::LocalPlayerTickEvent& event) {
                if (m_callbacksActive.load(std::memory_order_acquire)) observePlayer(event.player);
            });
    }
    if (m_containerSubscription == 0) {
        m_containerSubscription = events.subscribe<bedrocktools::events::ContainerSlotSelectedEvent>(
            [this](bedrocktools::events::ContainerSlotSelectedEvent& event) {
                if (m_callbacksActive.load(std::memory_order_acquire) &&
                    m_captureContainerEvents.load(std::memory_order_acquire) &&
                    event.afterSelection) {
                    m_captureRequested.store(true, std::memory_order_release);
                }
            });
    }
}

void ItemDuplicatorModule::unsubscribeEvents() {
    auto& events = bedrocktools::events::bus();
    if (m_tickSubscription != 0) {
        events.unsubscribe(m_tickSubscription);
        m_tickSubscription = 0;
    }
    if (m_containerSubscription != 0) {
        events.unsubscribe(m_containerSubscription);
        m_containerSubscription = 0;
    }
}

void ItemDuplicatorModule::onDisable() {
    m_callbacksActive.store(false, std::memory_order_release);
    unsubscribeEvents();
    flushLog();
    m_captureRequested.store(false, std::memory_order_release);
}

void ItemDuplicatorModule::onFrame() {
    if (!m_persistLogs.load(std::memory_order_acquire)) return;
    const auto now = std::chrono::steady_clock::now();
    if (m_nextFlush.time_since_epoch().count() == 0 || now >= m_nextFlush) {
        flushLog();
        m_nextFlush = now + std::chrono::seconds(5);
    }
}

void ItemDuplicatorModule::observePlayer(bedrocktools::sdk::Player* player) {
    if (!m_callbacksActive.load(std::memory_order_acquire)) return;

    // NormalTick is an actor hook. Resolve the actual local player from the live
    // ClientInstance instead of assuming the hook's actor argument is LocalPlayer.
    if (auto* client = bedrocktools::sdk::ClientInstance::current()) {
        if (auto* localPlayer = client->localPlayer()) player = localPlayer;
    }
    if (!player) return;

    if (m_duplicateMainHandRequested.exchange(false, std::memory_order_acq_rel)) {
        bedrocktools::core::InventoryAccess::get().copyMainHandToEmptyHotbar(player);
        m_captureRequested.store(true, std::memory_order_release);
    }

    const auto now = std::chrono::steady_clock::now();
    const bool requested = m_captureRequested.exchange(false, std::memory_order_acq_rel);
    if (!requested && m_nextCapture.time_since_epoch().count() != 0 && now < m_nextCapture) return;
    const int interval = std::clamp(m_captureIntervalMs.load(std::memory_order_acquire), 100, 2000);
    m_nextCapture = now + std::chrono::milliseconds(interval);

    std::vector<bedrocktools::core::InventorySlotSnapshot> slots;
    int selectedSlot = -1;
    std::string failureReason;
    const bool complete = bedrocktools::core::InventoryAccess::get().readInventorySnapshot(
        player, slots, &selectedSlot, &failureReason);

    Observation current;
    current.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_enabledAt).count();
    current.complete = complete;

    if (complete) {
        current.selectedSlot = selectedSlot;
        for (const auto& slot : slots) {
            if (slot.empty) continue;
            auto& total = current.itemTotals[slot.itemIdentifier];
            total += slot.count;
            if (slot.slotIndex == selectedSlot) current.itemIdentifier = slot.itemIdentifier;
        }
        if (!current.itemIdentifier.empty()) {
            const auto it = current.itemTotals.find(current.itemIdentifier);
            if (it != current.itemTotals.end() &&
                it->second <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                current.inventoryTotalForHeldIdentifier = static_cast<int>(it->second);
            }
        }
    } else {
        current.classification = "INCONCLUSIVE_READ_FAILURE";
        current.failureReason = failureReason.empty() ? "UNKNOWN_READ_FAILURE" : failureReason;
        bedrocktools::diagnostics::probe(
            bedrocktools::diagnostics::Level::Warning,
            "ItemDuplicator", "inventory_snapshot", current.failureReason,
            "read-only inventory snapshot rejected; no inventory mutation attempted");
    }

    {
        std::lock_guard<std::mutex> lock(m_recordsMutex);
        if (complete) {
            if (!m_hasPrevious) {
                current.classification = "BASELINE";
            } else {
                bool increased = false;
                for (const auto& [identifier, count] : current.itemTotals) {
                    const auto it = m_previous.itemTotals.find(identifier);
                    const std::uint64_t previous = it == m_previous.itemTotals.end() ? 0 : it->second;
                    if (count > previous) { increased = true; break; }
                }
                bool decreased = false;
                for (const auto& [identifier, count] : m_previous.itemTotals) {
                    const auto it = current.itemTotals.find(identifier);
                    const std::uint64_t currentCount = it == current.itemTotals.end() ? 0 : it->second;
                    if (currentCount < count) { decreased = true; break; }
                }

                if (!increased && !decreased) {
                    current.classification =
                        (current.selectedSlot != m_previous.selectedSlot ||
                         current.itemIdentifier != m_previous.itemIdentifier)
                        ? "SELECTION_CHANGED" : "NO_CHANGE";
                } else if (increased && decreased) {
                    current.classification = "LOCAL_INVENTORY_CHANGE";
                } else if (increased) {
                    current.classification = "LOCAL_COUNT_INCREASE";
                } else {
                    current.classification = "LOCAL_COUNT_DECREASE";
                }
            }
            m_previous = current;
            m_hasPrevious = true;
        }

        m_records.push_back(std::move(current));
        const auto limit = static_cast<std::size_t>(
            std::clamp(m_maxRecords.load(std::memory_order_acquire), 32, 1024));
        while (m_records.size() > limit) m_records.pop_front();
        m_logDirty = true;
        ++m_logGeneration;
    }
}

void ItemDuplicatorModule::trimRecords() {
    std::lock_guard<std::mutex> lock(m_recordsMutex);
    const auto limit = static_cast<std::size_t>(
        std::clamp(m_maxRecords.load(std::memory_order_acquire), 32, 1024));
    bool changed = false;
    while (m_records.size() > limit) {
        m_records.pop_front();
        changed = true;
    }
    if (changed) {
        m_logDirty = true;
        ++m_logGeneration;
    }
}

bool ItemDuplicatorModule::flushLog(bool force) {
    std::lock_guard<std::mutex> flushLock(m_flushMutex);
    if (!force && !m_persistLogs.load(std::memory_order_acquire)) return true;

    std::deque<Observation> records;
    std::string sessionId;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(m_recordsMutex);
        if (!m_logDirty && !force) return true;
        records = m_records;
        sessionId = m_sessionId;
        generation = m_logGeneration;
    }

    const std::string path = observerLogPath();
    std::error_code error;
    const auto parent = std::filesystem::path(path).parent_path();
    std::filesystem::create_directories(parent, error);
    if (error) return false;

    std::ostringstream output;
    for (const auto& record : records) {
        json line;
        line["session_id"] = sessionId;
        line["elapsed_ms"] = record.timestampMs;
        line["snapshot_complete"] = record.complete;
        line["selected_slot"] = record.selectedSlot;
        line["selected_item_identifier"] = record.itemIdentifier;
        line["selected_item_total"] = record.inventoryTotalForHeldIdentifier;
        line["classification"] = record.classification;
        if (!record.failureReason.empty()) line["failure_reason"] = record.failureReason;
        line["inventory_item_totals"] = record.itemTotals;
        output << line.dump() << '\n';
    }

    if (!bedrocktools::config::writePayloadAtomically(path, output.str())) {
        bedrocktools::diagnostics::log(bedrocktools::diagnostics::Level::Warning,
            "ItemDuplicator", "could not atomically write InventoryObserver.jsonl");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_recordsMutex);
        if (m_logGeneration == generation) m_logDirty = false;
    }
    return true;
}

void ItemDuplicatorModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("observer")
        .category("observer", "Inventory Observer", "Read-only inventory snapshots. Local changes are not proof of duplication.");

    auto addToggle = [&](const char* key, const char* title, const char* description, bool value) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.title = title;
        node.description = description;
        node.category = "observer";
        node.type = ConfigControlTypeV2::Toggle;
        node.defaultValue = value ? "true" : "false";
        node.currentValue = value ? "true" : "false";
        node.hasCurrentValue = true;
        schema.node(std::move(node));
    };
    auto addSlider = [&](const char* key, const char* title, const char* description,
                         int value, int minimum, int maximum, int step, const char* unit) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.title = title;
        node.description = description;
        node.category = "observer";
        node.type = ConfigControlTypeV2::SliderFloat;
        node.defaultValue = std::to_string(value);
        node.currentValue = std::to_string(value);
        node.hasCurrentValue = true;
        node.minValue = std::to_string(minimum);
        node.maxValue = std::to_string(maximum);
        node.step = std::to_string(step);
        node.unit = unit;
        schema.node(std::move(node));
    };

    addSlider("m_captureIntervalMs", "Capture interval", "Sampling interval for the local player inventory.", m_captureIntervalMs.load(), 100, 2000, 50, " ms");
    addSlider("m_maxRecords", "Maximum records", "Maximum in-memory and exported JSONL records.", m_maxRecords.load(), 32, 1024, 32, "");
    addToggle("m_captureContainerEvents", "Capture after container slot selection", "Request an extra snapshot after a container selection event.", m_captureContainerEvents.load());
    addToggle("m_persistLogs", "Persist observer log", "Atomically saves a bounded JSONL file in the active Bedaim config folder.", m_persistLogs.load());

    ConfigNodeV2 duplicateMainHand;
    duplicateMainHand.id = "m_duplicateMainHand";
    duplicateMainHand.key = "m_duplicateMainHand";
    duplicateMainHand.title = "Duplicate Main Hand (Creative test)";
    duplicateMainHand.description = "Attempts a native ItemStack copy into the first empty hotbar slot. Requires a verified Creative player and a readable Main Hand stack. This is a local client test, not a server dupe.";
    duplicateMainHand.category = "mainhand";
    duplicateMainHand.type = ConfigControlTypeV2::Button;
    duplicateMainHand.actionValue = "attempt";
    schema.node(std::move(duplicateMainHand));

    ConfigNodeV2 duplicateInfo;
    duplicateInfo.id = "m_duplicateMainHandInfo";
    duplicateInfo.title = "Main Hand test is Creative-only; survival and server-authority bypasses are not attempted.";
    duplicateInfo.description = "A local client-side copy does not prove that a server accepted or persisted another item.";
    duplicateInfo.category = "mainhand";
    duplicateInfo.type = ConfigControlTypeV2::Info;
    schema.node(std::move(duplicateInfo));

    addToggle("m_exportNow", "Export current observer log now", "One-shot action: writes the current records even when automatic persistence is disabled.", false);
    addToggle("m_clearSession", "Clear current observer session", "One-shot action: clears the in-memory ring and replaces the exported file with an empty log.", false);
    setConfigSchemaJson(moduleId, schema.toJson());
}

bool ItemDuplicatorModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "m_captureIntervalMs" || key == "m_maxRecords") {
        double parsed = 0.0;
        if (!parseNumber(value, parsed)) return false;
        if (key == "m_captureIntervalMs") {
            const double bounded = std::clamp(parsed, 100.0, 2000.0);
            m_captureIntervalMs.store(std::clamp(
                100 + static_cast<int>(std::lround((bounded - 100.0) / 50.0)) * 50, 100, 2000),
                std::memory_order_release);
        } else {
            const double bounded = std::clamp(parsed, 32.0, 1024.0);
            m_maxRecords.store(std::clamp(
                static_cast<int>(std::lround(bounded / 32.0)) * 32, 32, 1024),
                std::memory_order_release);
            trimRecords();
        }
        return true;
    }
    if (key == "m_captureContainerEvents") {
        bool parsed = false;
        if (!parseBool(value, parsed)) return false;
        m_captureContainerEvents.store(parsed, std::memory_order_release);
        return true;
    }
    if (key == "m_persistLogs") {
        bool parsed = false;
        if (!parseBool(value, parsed)) return false;
        m_persistLogs.store(parsed, std::memory_order_release);
        if (parsed) {
            std::lock_guard<std::mutex> lock(m_recordsMutex);
            m_logDirty = true;
            ++m_logGeneration;
        }
        return true;
    }
    if (key == "m_duplicateMainHand") {
        m_duplicateMainHandRequested.store(true, std::memory_order_release);
        bedrocktools::diagnostics::log(
            bedrocktools::diagnostics::Level::Info,
            "ItemDuplicator",
            "Main Hand Creative copy test queued; waiting for the next local-player tick");
        return true;
    }
    if (key == "m_exportNow") {
        bool parsed = false;
        if (!parseBool(value, parsed)) return false;
        if (parsed) flushLog(true);
        return true;
    }
    if (key == "m_clearSession") {
        bool parsed = false;
        if (!parseBool(value, parsed)) return false;
        if (parsed) {
            {
                std::lock_guard<std::mutex> lock(m_recordsMutex);
                m_records.clear();
                m_previous = {};
                m_hasPrevious = false;
                m_logDirty = true;
                ++m_logGeneration;
            }
            flushLog(true);
        }
        return true;
    }
    return false;
}

bool ItemDuplicatorModule::showInLegacyMenu(std::string_view) const {
    return true;
}

void ItemDuplicatorModule::loadConfig(const nlohmann::json& config) {
    Module::loadConfig(config);
    m_captureIntervalMs.store(configInteger(config, "m_captureIntervalMs", m_captureIntervalMs.load(), 100, 2000, 50),
                              std::memory_order_release);
    m_maxRecords.store(configInteger(config, "m_maxRecords", m_maxRecords.load(), 32, 1024, 32),
                       std::memory_order_release);
    m_persistLogs.store(configBool(config, "m_persistLogs", m_persistLogs.load()),
                        std::memory_order_release);
    m_captureContainerEvents.store(configBool(config, "m_captureContainerEvents", m_captureContainerEvents.load()),
                                   std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_recordsMutex);
        m_logDirty = false;
    }
}

void ItemDuplicatorModule::saveConfig(nlohmann::json& config) {
    Module::saveConfig(config);
    config["m_captureIntervalMs"] = m_captureIntervalMs.load(std::memory_order_acquire);
    config["m_maxRecords"] = m_maxRecords.load(std::memory_order_acquire);
    config["m_persistLogs"] = m_persistLogs.load(std::memory_order_acquire);
    config["m_captureContainerEvents"] = m_captureContainerEvents.load(std::memory_order_acquire);
    config["m_exportNow"] = false;
    config["m_clearSession"] = false;
}
