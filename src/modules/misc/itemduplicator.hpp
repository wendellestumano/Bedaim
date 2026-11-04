#pragma once

#include "../Module.hpp"

#include <bedrocktools/events/EventBus.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace bedrocktools::sdk { class Player; }

class ItemDuplicatorModule final : public Module {
public:
    ItemDuplicatorModule();

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    bool showInLegacyMenu(std::string_view key) const override;

    void loadConfig(const nlohmann::json& config) override;
    void saveConfig(nlohmann::json& config) override;

private:
    struct Observation {
        std::int64_t timestampMs = 0;
        int selectedSlot = -1;
        std::string itemIdentifier;
        int inventoryTotalForHeldIdentifier = -1;
        std::string classification;
    };

    void observePlayer(bedrocktools::sdk::Player* player);
    bool flushLog();
    void trimRecords();

    bedrocktools::events::Subscription m_tickSubscription = 0;
    std::deque<Observation> m_records;
    Observation m_previous;
    bool m_hasPrevious = false;

    int m_captureIntervalMs = 250;
    int m_maxRecords = 256;
    bool m_persistLogs = true;
    bool m_logDirty = false;

    std::chrono::steady_clock::time_point m_nextCapture{};
    std::chrono::steady_clock::time_point m_nextFlush{};
};
