#pragma once

#include <bedrocktools/sdk/Types.hpp>

#include <array>
#include <cstddef>
#include <utility>

namespace bedaim::esp_geometry {

using Vec3 = bedrocktools::sdk::Vec3;
using AABB = bedrocktools::sdk::AABB;
using Segment = std::pair<Vec3, Vec3>;

inline constexpr std::size_t kAabbEdgeCount = 12;
inline constexpr std::size_t kOuterRingSegmentCount = 60;
inline constexpr std::size_t kInnerRingSegmentCount = 68;

using AabbEdges = std::array<Segment, kAabbEdgeCount>;

struct TargetRingGeometry {
    std::array<Segment, kOuterRingSegmentCount> outer{};
    std::array<Segment, kInnerRingSegmentCount> inner{};
};

// Pure world-space geometry builders. They never access Minecraft objects or renderer hooks.
bool isValidBounds(const AABB& bounds);
bool buildAabbEdges(const AABB& bounds, AabbEdges& output);
bool buildTargetRing(const AABB& bounds,
                     const Vec3& cameraPosition,
                     bool animated,
                     float indicatorSize,
                     double elapsedSeconds,
                     TargetRingGeometry& output);

} // namespace bedaim::esp_geometry
