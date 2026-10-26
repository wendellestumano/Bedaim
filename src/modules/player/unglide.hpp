#pragma once
#include "../Module.hpp"
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
namespace bedrocktools::sdk { class Player; }
class UnglideModule final : public Module {
public:
 UnglideModule(); void onInit() override; void onEnable() override; void onDisable() override;
 void loadConfig(const nlohmann::json&) override; void saveConfig(nlohmann::json&) override;
 static std::optional<bool> queryGlidingState(void* player);
 void onMenuRegistered() override; bool onMenuConfigChanged(std::string_view,std::string_view) override;
 bool m_unglideEnabled=false; float m_targetDistance=8.0f; float m_maxVerticalDifference=4.0f; int m_burstCount=3; int m_pressIntervalMs=80;
private:
 void processPlayerTick(bedrocktools::sdk::Player*);
 std::mutex m_stateMutex; std::chrono::steady_clock::time_point m_nextAttempt{},m_nextDiagnostic{};
 // Protects the state while the potentially blocking JNI broadcast runs without the mutex.
 bool m_dispatchInProgress=false;
 std::uint64_t m_dispatchToken=0;
 int m_burstRemaining=0;
 enum class TriggerState { Idle, Tracking, Triggered };
 TriggerState m_triggerState=TriggerState::Idle;
 void* m_latchedTarget=nullptr;
 bool m_targetLatched=false;
};
