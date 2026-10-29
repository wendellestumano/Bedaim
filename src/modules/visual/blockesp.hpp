#pragma once
#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

class BlockEspModule final : public Module {
public:
    enum class Kind : std::uint8_t { Shulker, Chest, EnderChest, Hopper, Dispenser, Dropper, Barrel, Furnace, Utility, Count };
    struct Outline {
        bedrocktools::sdk::BlockPos position{};
        std::uint32_t color = 0xFFFFFFFFu;
        Kind kind = Kind::Utility;
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
    bool showHoppers = true;
    bool showDispensers = true;
    bool showDroppers = true;
    bool showBarrels = true;
    bool showFurnaces = true;
    bool showUtilityBlocks = true;
    int horizontalRadius = 16;
    int verticalRadius = 8;
    int scanBudgetPerTick = 1024;
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
    static PositionKey keyFor(const bedrocktools::sdk::BlockPos& pos) noexcept;
    static bool classify(std::string_view name, Kind& kind);
    static std::uint32_t defaultColor(Kind kind) noexcept;
    bool isKindEnabled(Kind kind) const noexcept;
    std::uint32_t colorFor(Kind kind) const noexcept;
    void clearCache();

    bedrocktools::sdk::BlockSource* m_blockSource = nullptr;
    bedrocktools::sdk::BlockPos m_scanCenter{};
    bedrocktools::sdk::BlockPos m_lastCenter{};
    bool m_scanActive = false;
    bool m_hasCenter = false;
    std::size_t m_scanCursor = 0;
    std::size_t m_scanTotal = 0;
    std::uint64_t m_tickCount = 0;
    std::uint64_t m_lastResolveAttemptTick = 0;
    mutable std::mutex m_cacheMutex;
    std::unordered_map<PositionKey, Outline, PositionHash> m_cache;
};
