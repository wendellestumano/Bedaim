#include "modules/misc/SeedMapEngine.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using namespace bedaim::seedmap;

    // A fixed Bedrock seed must produce deterministic, bounded candidates.
    const auto first = findStructureAttempts(
        1234567890123456789ULL, Dimension::Overworld, 0, 0, 8192, 256);
    const auto second = findStructureAttempts(
        1234567890123456789ULL, Dimension::Overworld, 0, 0, 8192, 256);
    assert(!first.empty());
    assert(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        assert(first[i].x == second[i].x);
        assert(first[i].z == second[i].z);
        assert(first[i].distance == second[i].distance);
        assert(first[i].distance <= 8192.0f);
        assert(!first[i].possibleTypes.empty());
    }

    // Fixed oracle point: SeedForge's Bedrock MT19937/region formula gives
    // the village attempt at (264, 232) for this seed in region (0, 0).
    const auto oracle = findStructureAttempts(
        123456789ULL, Dimension::Overworld, 0, 0, 8192, 256);
    bool foundVillageOracle = false;
    for (const auto& marker : oracle) {
        if (marker.x == 264 && marker.z == 232) {
            foundVillageOracle = true;
            break;
        }
    }
    assert(foundVillageOracle);

    // A seed change must affect at least one candidate in a nontrivial window.
    const auto other = findStructureAttempts(
        987654321ULL, Dimension::Overworld, 0, 0, 8192, 256);
    bool differs = first.size() != other.size();
    for (std::size_t i = 0; !differs && i < first.size(); ++i)
        differs = first[i].x != other[i].x || first[i].z != other[i].z;
    assert(differs);

    const auto nether = findStructureAttempts(
        1234567890123456789ULL, Dimension::Nether, 0, 0, 8192, 256);
    for (const auto& marker : nether) {
        for (const auto type : marker.possibleTypes) {
            assert(type == StructureType::Fortress ||
                   type == StructureType::Bastion ||
                   type == StructureType::NetherRuinedPortal);
        }
    }

    const auto end = findStructureAttempts(
        1234567890123456789ULL, Dimension::End, 0, 0, 8192, 256);
    for (const auto& marker : end) {
        for (const auto type : marker.possibleTypes)
            assert(type == StructureType::EndCity);
    }

    assert(findStructureAttempts(1, Dimension::Overworld, 0, 0, 0).empty());
    assert(findStructureAttempts(1, Dimension::Overworld, 0, 0, 512, 0).empty());

    std::cout << "SeedMap engine tests passed\n";
    return 0;
}
