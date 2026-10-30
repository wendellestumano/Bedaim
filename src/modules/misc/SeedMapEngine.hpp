#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bedaim::seedmap {

enum class Dimension : int {
    Overworld = 0,
    Nether = 1,
    End = 2,
};

enum class StructureType : int {
    Village,
    Mansion,
    EndCity,
    Monument,
    AncientCity,
    Outpost,
    BuriedTreasure,
    OceanRuin,
    Shipwreck,
    Fortress,
    Bastion,
    DesertPyramid,
    Igloo,
    SwampHut,
    JungleTemple,
    RuinedPortal,
    NetherRuinedPortal,
};

struct StructureMarker {
    int x{};
    int z{};
    float distance{};
    std::vector<StructureType> possibleTypes;
    bool biomeConfirmed{false};
};

std::string_view structureName(StructureType type) noexcept;
std::string_view structureShortName(StructureType type) noexcept;

// Returns deterministic Bedrock structure-generation attempts around the
// player's current position. These are candidate sites; biome/terrain
// confirmation is a separate stage and is deliberately not implied here.
std::vector<StructureMarker> findStructureAttempts(
    std::uint64_t worldSeed,
    Dimension dimension,
    int centerX,
    int centerZ,
    int radiusBlocks,
    std::size_t maxMarkers = 512);

} // namespace bedaim::seedmap
