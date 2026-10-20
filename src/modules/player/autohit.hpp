#pragma once

#include "../Module.hpp"
#include "AutoHitGuards.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>

namespace bedrocktools::sdk { class Player; }

class AutoHitModule final : public Module {
public:
    AutoHitModule();

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    bool onMouseEvent(int button, bool isDown) override;
    bool onKeyEvent(int keyCode, bool isDown, unsigned int unicodeChar);
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

private:
    void processPlayerTick(bedrocktools::sdk::Player* player);

    std::mutex m_stateMutex;
    // Emits gate diagnostics only when the pipeline changes state, avoiding per-tick log spam.
    std::atomic<int> m_lastGateDecision{-1};
    std::atomic_bool m_containerOpen{false};
    std::atomic_bool m_chatOpen{false};
    float m_attackRange = 3.0f;

    // Weapon profiles default to swords only. Each category has its own
    // enable switch and CPS cap; unknown/non-weapon items never dispatch attacks.
    bool m_swordEnabled = true;
    float m_swordCps = 8.0f;
    bool m_axeEnabled = false;
    float m_axeCps = 4.0f;
    bool m_maceEnabled = false;
    float m_maceCps = 2.0f;
    bool m_tridentEnabled = false;
    float m_tridentCps = 3.0f;

    std::chrono::steady_clock::time_point m_nextAttack{};
    // When the native attack route is unavailable, do not repeat the resolver/inventory path every player tick.
    std::chrono::steady_clock::time_point m_nextNativeRouteProbe{};
    // Stop repeated attacks against one actor if the validated native wrapper
    // accepts them but local target hurt-time never shows a positive transition.
    bedaim::autohit::NoHitFeedbackGuard m_noHitFeedbackGuard;
};
