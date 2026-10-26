#pragma once

#include "../Module.hpp"
#include "AutoHitWeaponProfile.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bedrocktools::sdk { class Player; }

class AutoHitModule final : public Module {
public:
    AutoHitModule();

    void onInit() override;
    // Lets coordinated combat modules verify that the exact held item is enabled
    // in AutoHit's persisted profile list before entering an attack window.
    bool hasEnabledProfileFor(std::string_view identifier);
    void onEnable() override;
    void onDisable() override;
    bool onMouseEvent(int button, bool isDown) override;
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
    float m_swordCps = 5.0f;
    bool m_axeEnabled = false;
    float m_axeCps = 3.0f;
    bool m_maceEnabled = false;
    float m_maceCps = 1.5f;
    bool m_tridentEnabled = false;
    float m_tridentCps = 2.5f;

    // Item-specific profiles override the legacy weapon-class defaults above.
    // The profile list is serialized in config.json and is intentionally bounded.
    std::vector<bedaim::autohit::weapon::CustomProfile> m_customWeaponProfiles;
    std::string m_addWeaponChoice = "minecraft:mace";
    std::string m_customWeaponIdentifier;
    std::string m_selectedProfileIdentifier;

    // Initial burst is tied to a held item + target pair so it cannot re-arm every tick.
    std::string m_burstWeaponIdentifier;
    void* m_burstTarget = nullptr;
    int m_burstRemaining = 0;

    std::chrono::steady_clock::time_point m_nextAttack{};
};
