#pragma once

#include "../Module.hpp"
#include "xray_block_policy.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/events/GameModeActionEvent.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

class OreEspModule final : public Module {
public:
    struct Outline {
        bedrocktools::sdk::BlockPos position{};
        std::uint32_t color = 0xFFFFFFFFu;
        bedaim::xray::OreKind kind = bedaim::xray::OreKind::None;
    };

    OreEspModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Initial and movement-triggered scans are budgeted; confirmed block changes
    // queue local refreshes. The world is never rescanned in full every tick.
    void onPlayerTick(bedrocktools::sdk::Player* player);
    void onGameModeAction(bedrocktools::events::GameModeActionEvent& event);
    void getRenderSnapshot(const bedrocktools::sdk::Vec3& playerPosition,
                           std::vector<Outline>& output) const;

    bool showDiamonds = true;
    bool showEmeralds = true;
    bool showGold = true;
    bool showIron = true;
    bool showCopper = true;
    bool showRedstone = true;
    bool showLapis = true;
    bool showQuartz = true;
    bool showCoal = false;
    bool showAncientDebris = true;
    bool showOtherOres = true;
    int horizontalRadius = 48;
    int verticalRadius = 24;
    int scanBudgetPerTick = 512;
    float outlineThickness = 0.04f;

private:
    struct PositionKey {
        int x = 0, y = 0, z = 0;
        bool operator==(const PositionKey&) const = default;
    };
    struct PositionHash {
        std::size_t operator()(const PositionKey& p) const noexcept;
    };
    struct PendingBreak {
        bedrocktools::sdk::BlockPos position{};
        std::uint64_t initialNameHash = 0;
        bool hasBaseline = false;
        unsigned ageTicks = 0;
    };
    struct ScanJob {
        bedrocktools::sdk::BlockPos center{};
    };
    struct BlockIdentity {
        bool valid = false;
        bool air = false;
        std::uint64_t nameHash = 0;
        bedaim::xray::OreKind kind = bedaim::xray::OreKind::None;
    };

    static PositionKey keyFor(const bedrocktools::sdk::BlockPos& pos) noexcept;
    BlockIdentity inspectBlock(bedrocktools::sdk::BlockSource* source,
                               const bedrocktools::sdk::BlockPos& pos) const;
    bool isKindEnabled(bedaim::xray::OreKind kind) const noexcept;
    void queueScan(const bedrocktools::sdk::BlockPos& center);
    void startNextScan();
    void processScanBatch(bedrocktools::sdk::BlockSource* source);
    void eraseCachedPosition(const bedrocktools::sdk::BlockPos& pos);
    void clearWorldState();

    bedrocktools::sdk::BlockSource* m_blockSource = nullptr;
    std::vector<PendingBreak> m_pendingBreaks;
    std::deque<ScanJob> m_scanQueue;
    bedrocktools::sdk::BlockPos m_scanCenter{};
    std::size_t m_scanCursor = 0;
    std::size_t m_scanTotal = 0;
    int m_scanRadiusHorizontal = 0;
    int m_scanRadiusVertical = 0;
    bool m_scanActive = false;
    bool m_hasScannedAtLeastOnce = false;
    bool m_hasAutoScanCenter = false;
    bedrocktools::sdk::BlockPos m_lastAutoScanCenter{};
    bool m_rescanRequested = false;
    std::uint64_t m_tickCount = 0;
    std::uint64_t m_lastResolveAttemptTick = 0;

    mutable std::mutex m_cacheMutex;
    std::unordered_map<PositionKey, Outline, PositionHash> m_oreCache;
};