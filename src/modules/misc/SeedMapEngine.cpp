#include "SeedMapEngine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace bedaim::seedmap {
namespace {

enum class Spread : std::uint8_t { Linear, Triangular };
enum class Pick : std::uint8_t { Any, Fortress, Bastion };

struct BedrockConfig {
    StructureType type;
    Dimension dimension;
    std::uint32_t salt;
    int spacing;
    int separation;
    Spread spread;
    Pick pick;
};

// Bedrock region-placement parameters. The Bedrock RNG is MT19937 seeded per
// region; see the MIT-licensed SeedForge implementation and notice in
// third_party/seedforge/LICENSE for provenance and verification context.
constexpr std::array<BedrockConfig, 17> kConfigs{{
    {StructureType::Village, Dimension::Overworld, 10387312u, 34, 8, Spread::Triangular, Pick::Any},
    {StructureType::Mansion, Dimension::Overworld, 10387319u, 80, 20, Spread::Triangular, Pick::Any},
    {StructureType::EndCity, Dimension::End, 10387313u, 20, 11, Spread::Triangular, Pick::Any},
    {StructureType::Monument, Dimension::Overworld, 10387313u, 32, 5, Spread::Triangular, Pick::Any},
    {StructureType::AncientCity, Dimension::Overworld, 20083232u, 24, 8, Spread::Triangular, Pick::Any},
    {StructureType::Outpost, Dimension::Overworld, 165745296u, 80, 24, Spread::Triangular, Pick::Any},
    {StructureType::BuriedTreasure, Dimension::Overworld, 16842397u, 4, 2, Spread::Triangular, Pick::Any},
    {StructureType::OceanRuin, Dimension::Overworld, 14357621u, 20, 8, Spread::Linear, Pick::Any},
    {StructureType::Shipwreck, Dimension::Overworld, 165745295u, 24, 4, Spread::Linear, Pick::Any},
    {StructureType::Fortress, Dimension::Nether, 30084232u, 30, 4, Spread::Linear, Pick::Fortress},
    {StructureType::Bastion, Dimension::Nether, 30084232u, 30, 4, Spread::Linear, Pick::Bastion},
    {StructureType::DesertPyramid, Dimension::Overworld, 14357617u, 32, 8, Spread::Linear, Pick::Any},
    {StructureType::Igloo, Dimension::Overworld, 14357617u, 32, 8, Spread::Linear, Pick::Any},
    {StructureType::SwampHut, Dimension::Overworld, 14357617u, 32, 8, Spread::Linear, Pick::Any},
    {StructureType::JungleTemple, Dimension::Overworld, 14357617u, 32, 8, Spread::Linear, Pick::Any},
    {StructureType::RuinedPortal, Dimension::Overworld, 40552231u, 40, 15, Spread::Linear, Pick::Any},
    {StructureType::NetherRuinedPortal, Dimension::Nether, 40552231u, 25, 10, Spread::Linear, Pick::Any},
}};

struct Mt19937 {
    static constexpr int N = 624;
    static constexpr int M = 397;
    std::array<std::uint32_t, N> state{};
    int index = N;

    explicit Mt19937(std::uint32_t seed) {
        state[0] = seed;
        for (int i = 1; i < N; ++i) {
            state[static_cast<std::size_t>(i)] =
                1812433253u * (state[static_cast<std::size_t>(i - 1)] ^
                    (state[static_cast<std::size_t>(i - 1)] >> 30)) +
                static_cast<std::uint32_t>(i);
        }
    }

    void twist() {
        constexpr std::uint32_t upper = 0x80000000u;
        constexpr std::uint32_t lower = 0x7fffffffu;
        constexpr std::uint32_t matrix = 0x9908b0dfu;
        for (int i = 0; i < N; ++i) {
            const std::uint32_t y =
                (state[static_cast<std::size_t>(i)] & upper) |
                (state[static_cast<std::size_t>((i + 1) % N)] & lower);
            std::uint32_t next =
                state[static_cast<std::size_t>((i + M) % N)] ^ (y >> 1);
            if ((y & 1u) != 0) next ^= matrix;
            state[static_cast<std::size_t>(i)] = next;
        }
        index = 0;
    }

    std::uint32_t next() {
        if (index >= N) twist();
        std::uint32_t y = state[static_cast<std::size_t>(index++)];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= y >> 18;
        return y;
    }
};

int floorDiv(int value, int divisor) noexcept {
    int quotient = value / divisor;
    const int remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) --quotient;
    return quotient;
}

bool generateAttempt(const BedrockConfig& config, std::uint64_t seed,
                     int regionX, int regionZ, int& outX, int& outZ) {
    const auto range = static_cast<std::uint32_t>(config.spacing - config.separation);
    const std::uint32_t base =
        static_cast<std::uint32_t>(regionX) * 2570712328u +
        static_cast<std::uint32_t>(regionZ) * 4048968661u +
        config.salt;
    Mt19937 rng(static_cast<std::uint32_t>(seed) + base);

    std::uint32_t ox = 0;
    std::uint32_t oz = 0;
    if (range != 0 && config.spread == Spread::Triangular) {
        const std::uint32_t t0 = rng.next() % range;
        const std::uint32_t t1 = rng.next() % range;
        const std::uint32_t t2 = rng.next() % range;
        const std::uint32_t t3 = rng.next() % range;
        ox = (t0 + t1) / 2;
        oz = (t2 + t3) / 2;
    } else if (range != 0) {
        ox = rng.next() % range;
        oz = rng.next() % range;
    }

    if (config.pick != Pick::Any) {
        const bool bastion = (rng.next() % 6u) >= 2u;
        if (bastion != (config.pick == Pick::Bastion)) return false;
    }

    const std::int64_t chunkX =
        static_cast<std::int64_t>(regionX) * config.spacing + ox;
    const std::int64_t chunkZ =
        static_cast<std::int64_t>(regionZ) * config.spacing + oz;
    const std::int64_t blockX = chunkX * 16 + 8;
    const std::int64_t blockZ = chunkZ * 16 + 8;
    if (blockX < std::numeric_limits<int>::min() ||
        blockX > std::numeric_limits<int>::max() ||
        blockZ < std::numeric_limits<int>::min() ||
        blockZ > std::numeric_limits<int>::max()) return false;

    outX = static_cast<int>(blockX);
    outZ = static_cast<int>(blockZ);
    return true;
}

} // namespace

std::string_view structureName(StructureType type) noexcept {
    switch (type) {
        case StructureType::Village: return "Village";
        case StructureType::Mansion: return "Woodland Mansion";
        case StructureType::EndCity: return "End City";
        case StructureType::Monument: return "Ocean Monument";
        case StructureType::AncientCity: return "Ancient City";
        case StructureType::Outpost: return "Pillager Outpost";
        case StructureType::BuriedTreasure: return "Buried Treasure";
        case StructureType::OceanRuin: return "Ocean Ruins";
        case StructureType::Shipwreck: return "Shipwreck";
        case StructureType::Fortress: return "Nether Fortress";
        case StructureType::Bastion: return "Bastion";
        case StructureType::DesertPyramid: return "Desert Temple";
        case StructureType::Igloo: return "Igloo";
        case StructureType::SwampHut: return "Witch Hut";
        case StructureType::JungleTemple: return "Jungle Temple";
        case StructureType::RuinedPortal: return "Ruined Portal";
        case StructureType::NetherRuinedPortal: return "Nether Ruined Portal";
    }
    return "Unknown";
}

std::string_view structureShortName(StructureType type) noexcept {
    switch (type) {
        case StructureType::Village: return "V";
        case StructureType::Mansion: return "M";
        case StructureType::EndCity: return "E";
        case StructureType::Monument: return "O";
        case StructureType::AncientCity: return "A";
        case StructureType::Outpost: return "P";
        case StructureType::BuriedTreasure: return "T";
        case StructureType::OceanRuin: return "R";
        case StructureType::Shipwreck: return "S";
        case StructureType::Fortress: return "F";
        case StructureType::Bastion: return "B";
        case StructureType::DesertPyramid: return "D";
        case StructureType::Igloo: return "I";
        case StructureType::SwampHut: return "W";
        case StructureType::JungleTemple: return "J";
        case StructureType::RuinedPortal: return "N";
        case StructureType::NetherRuinedPortal: return "N";
    }
    return "?";
}

std::vector<StructureMarker> findStructureAttempts(
    std::uint64_t worldSeed, Dimension dimension, int centerX, int centerZ,
    int radiusBlocks, std::size_t maxMarkers) {
    std::vector<StructureMarker> markers;
    if (radiusBlocks <= 0 || maxMarkers == 0) return markers;

    const std::int64_t radiusSquared =
        static_cast<std::int64_t>(radiusBlocks) * radiusBlocks;

    for (const auto& config : kConfigs) {
        if (config.dimension != dimension) continue;
        const int regionBlocks = config.spacing * 16;
        const int rx0 = floorDiv(centerX - radiusBlocks, regionBlocks);
        const int rx1 = floorDiv(centerX + radiusBlocks, regionBlocks);
        const int rz0 = floorDiv(centerZ - radiusBlocks, regionBlocks);
        const int rz1 = floorDiv(centerZ + radiusBlocks, regionBlocks);

        for (int rz = rz0; rz <= rz1; ++rz) {
            for (int rx = rx0; rx <= rx1; ++rx) {
                int x = 0;
                int z = 0;
                if (!generateAttempt(config, worldSeed, rx, rz, x, z)) continue;

                const std::int64_t dx = static_cast<std::int64_t>(x) - centerX;
                const std::int64_t dz = static_cast<std::int64_t>(z) - centerZ;
                const std::int64_t distanceSquared = dx * dx + dz * dz;
                if (distanceSquared > radiusSquared) continue;

                auto found = std::find_if(markers.begin(), markers.end(),
                    [x, z](const StructureMarker& marker) {
                        return marker.x == x && marker.z == z;
                    });
                if (found == markers.end()) {
                    StructureMarker marker;
                    marker.x = x;
                    marker.z = z;
                    marker.distance = static_cast<float>(std::sqrt(
                        static_cast<double>(distanceSquared)));
                    marker.possibleTypes.push_back(config.type);
                    markers.push_back(std::move(marker));
                } else if (std::find(found->possibleTypes.begin(),
                                     found->possibleTypes.end(), config.type) ==
                           found->possibleTypes.end()) {
                    found->possibleTypes.push_back(config.type);
                }
            }
        }
    }

    std::sort(markers.begin(), markers.end(),
        [](const StructureMarker& a, const StructureMarker& b) {
            return a.distance < b.distance;
        });
    if (markers.size() > maxMarkers) markers.resize(maxMarkers);
    return markers;
}

} // namespace bedaim::seedmap
