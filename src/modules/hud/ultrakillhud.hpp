#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

class UltrakillHudModule : public Module {
public:
    UltrakillHudModule();

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    void onLocalPlayerTick(bedrocktools::sdk::Player* player);
    void onAttack(bedrocktools::sdk::Actor* target);
    bool isMaceEquipped(bedrocktools::sdk::Player* player) const;

    std::atomic<bedrocktools::sdk::Player*> m_localPlayer{nullptr};
    std::atomic_bool m_maceEquipped{false};
    std::atomic<float> m_fallSpeedBps{0.0f};

    std::mutex m_stateMutex;
    bool m_hasLastY = false;
    float m_lastY = 0.0f;

    bedrocktools::sdk::Actor* m_pendingTarget = nullptr;
    int m_pendingHurtTime = 0;
    std::chrono::steady_clock::time_point m_pendingDeadline{};
    bool m_hasPendingTarget = false;

    std::uint64_t m_styleScore = 0;
    unsigned int m_combo = 0;
    std::chrono::steady_clock::time_point m_lastConfirmedHit{};
    std::chrono::steady_clock::time_point m_flashStarted{};
    float m_impactFallSpeedBps = 0.0f;
    bool m_flashActive = false;

    float hudPosX = 18.0f;
    float hudPosY = 18.0f;
    bool isHudModule = true;

    bool m_showMaceStatus = true;
    bool m_showStyleCounter = true;
    bool m_showReticle = true;
    bool m_enableParryFlash = true;
    bool m_enableImpactShake = true;
    bool m_showImpactText = true;
};
