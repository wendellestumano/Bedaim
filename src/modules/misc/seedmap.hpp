#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/network/Packet.hpp>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

class SeedMapModule final : public Module {
public:
    SeedMapModule();

    void onInit() override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

    void onPlayerTick(bedrocktools::sdk::Player* player);
    void observeTextPacket(const bedrocktools::sdk::TextPacket& packet);

private:
    bool installTextHook();
    bool openSeedMap();
    static std::string parseSeedAnnouncement(std::string_view message);
    static std::string urlEncode(std::string_view value);
    static bool openUrlInBrowser(std::string_view url);

    mutable std::mutex m_stateMutex;
    std::string m_seed;
    std::string m_version{"26.50"};
    std::string m_dimension{"overworld"};
    int m_playerX = 0;
    int m_playerZ = 0;
    bool m_hasPlayerPosition = false;
};
