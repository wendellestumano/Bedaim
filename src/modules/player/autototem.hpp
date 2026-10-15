#pragma once

#include "../Module.hpp"

#include <atomic>
#include <cstdint>
#include <chrono>
#include <mutex>

namespace bedrocktools::sdk {
class Player;
}

class AutoTotemModule final : public Module {
public:
    enum class UiSequencePhase : std::uint8_t {
        Idle,
        WaitForOpen,
        WaitBeforeTransfer,
        WaitBeforeClose,
        WaitForClose
    };
    AutoTotemModule();

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

private:
    void processPlayerTick(bedrocktools::sdk::Player* player);

    mutable std::mutex m_stateMutex;
    std::atomic_bool m_containerOpen{false};
    std::atomic_bool m_manualRequest{false};
    bool m_manualSequenceActive = false;
    bool m_pendingRefill = false;
    bool m_lastOffhandHadTotem = false;
    std::chrono::steady_clock::time_point m_refillAfter{};
    std::chrono::steady_clock::time_point m_nextInventoryScan{};
    std::chrono::steady_clock::time_point m_nextOffhandCheck{};
    std::chrono::steady_clock::time_point m_uiPhaseDeadline{};
    int m_cachedTotemSlot = -1;
    UiSequencePhase m_uiSequencePhase = UiSequencePhase::Idle;
    bool m_uiTransferSucceeded = false;

    float m_delayMs = 100.0f;
    // Inventory ItemStack descriptions are expensive on mobile. Retry absent
    // totems less often, while still detecting a consumed offhand totem quickly.
    float m_scanRetryIntervalMs = 400.0f;
    float m_offhandPollIntervalMs = 150.0f;
    float m_uiStepDelayMs = 80.0f;
    bool m_useInventoryUiSequence = true;
    bool m_combatOnly = false;
    bool m_keepTotemEquipped = true;
};
