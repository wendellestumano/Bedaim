#pragma once

#include "../Module.hpp"
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
    bool m_attackHeld = false;
    // Mobile-friendly default: valid targets and weapon profiles can drive AutoHit
    // without relying on the launcher mouse bridge. Desktop users may require input.
    bool m_requireAttackInput = false;
    bool m_captureNextInput = false;
    std::chrono::steady_clock::time_point m_captureArmedAt{};
    bool m_keyboardAttackHeld = false;
    int m_attackKeyCode = -1;
    std::atomic_bool m_containerOpen{false};
    std::atomic_bool m_chatOpen{false};
    int m_heldButton = -1;
    int m_attackButton = 0;
    float m_attackRange = 3.2f;
    bool m_touchRepeatEnabled = true;
    int m_touchRepeatWindowMs = 300;
    std::chrono::steady_clock::time_point m_touchRepeatUntil{};

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
};
