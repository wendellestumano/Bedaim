#pragma once
#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class BlockEspModule final : public Module {
public:
    enum class Kind : std::uint8_t { Shulker, Chest, EnderChest, Vault, Spawner, Hopper, Dispenser, Dropper, Barrel, Furnace, Utility, Count };
    struct Outline {
        bedrocktools::sdk::BlockPos position{};
        std::uint32_t color = 0xFFFFFFFFu;
        Kind kind = Kind::Utility;
        std::string blockName;
    };

    BlockEspModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onMenuRegistered() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;
    void onPlayerTick(bedrocktools::sdk::Player* player);
    void getRenderSnapshot(const bedrocktools::sdk::Vec3& playerPosition, std::vector<Outline>& output) const;

    bool showTracer = true;
    bool showShulkers = true;
    bool showChests = true;
    bool showEnderChests = true;
    bool showVaults = true;
    bool showSpawners = true;
    bool showHoppers = true;
    bool showDispensers = true;
    bool showDroppers = true;
    bool showBarrels = true;
    bool showFurnaces = true;
    bool showUtilityBlocks = true;
    bool maxScanRadius = false;
    int horizontalRadius = 320;
    int verticalRadius = 80;
    int scanBudgetPerTick = 16384;
    float outlineThickness = 0.035f;
    float tracerThickness = 0.035f;
    std::uint32_t shulkerColor = 0xFFFF40FFu;
    std::uint32_t chestColor = 0xFFFFB52Eu;
    std::uint32_t enderChestColor = 0xFFB45CFFu;
    std::uint32_t hopperColor = 0xFF55DDE0u;
    std::uint32_t dispenserColor = 0xFF65A5FFu;
    std::uint32_t dropperColor = 0xFF4E78D2u;
    std::uint32_t barrelColor = 0xFFB77A42u;
    std::uint32_t furnaceColor = 0xFFFF6655u;
    std::uint32_t utilityColor = 0xFF8DDF73u;
    std::uint32_t tracerColor = 0xFFFFFFFFu;

private:
    struct PositionKey {
        int x = 0, y = 0, z = 0;
        bool operator==(const PositionKey&) const = default;
    };
    struct PositionHash {
        std::size_t operator()(const PositionKey& p) const noexcept;
    };
    struct QuerySlot {
        PositionKey position{};
        std::uint32_t generation = 0;
    };
    bool markQueryPosition(const PositionKey& position) noexcept;
    static PositionKey keyFor(const bedrocktools::sdk::BlockPos& pos) noexcept;
    static bool classify(std::string_view name, Kind& kind);
    static std::uint32_t defaultColor(Kind kind) noexcept;
    bool isKindEnabled(Kind kind) const noexcept;
    bool isBlockEnabled(const Outline& outline) const;
    std::uint32_t colorFor(Kind kind) const noexcept;
    std::uint32_t colorForBlock(Kind kind, std::string_view blockName) const noexcept;
    void clearCache();

    bedrocktools::sdk::BlockSource* m_blockSource = nullptr;
    bedrocktools::sdk::BlockPos m_scanCenter{};
    bedrocktools::sdk::BlockPos m_lastCenter{};
    bool m_scanActive = false;
    bool m_hasCenter = false;
    std::size_t m_scanCursor = 0;
    std::size_t m_scanTotal = 0;
    std::vector<std::pair<int, int>> m_horizontalScanOffsets;
    // Rolling local volume: 32x160x32 blocks, processed incrementally across ticks.
    std::vector<bedrocktools::sdk::BlockPos> m_localScanOffsets;
    bedrocktools::sdk::BlockPos m_localScanOrigin{};
    std::size_t m_localScanCursor = 0;
    bool m_localScanActive = false;
    bool m_hasLocalScanOrigin = false;
    // A short, rolling priority scan follows the player independently of the long-range scan.
    std::vector<bedrocktools::sdk::BlockPos> m_priorityScanOffsets;
    bedrocktools::sdk::BlockPos m_priorityScanOrigin{};
    std::size_t m_priorityScanCursor = 0;
    bool m_priorityScanActive = false;
    bool m_hasPriorityCenter = false;
    bool m_priorityDirectionDirty = false;
    bedrocktools::sdk::Vec3 m_previousPlayerPosition{};
    bool m_hasPreviousPlayerPosition = false;
    bool m_hasMovementDirection = false;
    float m_moveDirectionX = 0.0f;
    float m_moveDirectionZ = 1.0f;
    std::uint64_t m_tickCount = 0;
    std::uint64_t m_lastResolveAttemptTick = 0;
    mutable std::mutex m_cacheMutex;
    std::unordered_map<PositionKey, Outline, PositionHash> m_cache;
    // Fixed open-addressing table avoids per-block allocations while deduplicating
    // overlapping scan passes. 32768 slots covers the maximum 16384 queries/tick.
    std::vector<QuerySlot> m_queryDedupSlots;
    std::uint32_t m_queryGeneration = 0;
    std::size_t m_uniqueQueriesThisTick = 0;
    std::unordered_map<std::string, bool> m_utilityFilters;
};
