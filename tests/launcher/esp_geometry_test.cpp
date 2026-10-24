#include "../../src/modules/visual/esp_geometry.hpp"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;
int checks = 0;

void check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << "\n";
    }
}

bool near(float a, float b, float epsilon = 0.0001f) {
    return std::fabs(a - b) <= epsilon;
}

bool same(const bedrocktools::sdk::Vec3& a, const bedrocktools::sdk::Vec3& b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

bool finite(const bedrocktools::sdk::Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
}

int main() {
    using namespace bedrocktools::sdk;
    using namespace bedaim::esp_geometry;

    const bedaim::esp_geometry::AABB box{{-1.0f, 0.0f, -1.0f}, {1.0f, 2.0f, 1.0f}};
    AabbEdges edges{};
    check(isValidBounds(box), "valid AABB accepted");
    check(buildAabbEdges(box, edges), "AABB edges generated");
    check(edges.size() == 12, "AABB has twelve edges");
    check(same(edges[0].first, box.min), "first edge starts at minimum corner");
    check(same(edges[0].second, Vec3{box.max.x, box.min.y, box.min.z}),
          "first edge ends at the expected corner");
    for (const auto& segment : edges) {
        check(finite(segment.first) && finite(segment.second), "AABB line endpoints are finite");
    }

    const bedaim::esp_geometry::AABB invalid{{0, 0, 0}, {0, 1, 1}};
    check(!isValidBounds(invalid), "zero-width AABB rejected");
    check(!buildAabbEdges(invalid, edges), "invalid AABB does not generate lines");

    const Vec3 camera{0.0f, 1.2f, -4.0f};
    TargetRingGeometry staticRingA{}, staticRingB{}, animatedRingA{}, animatedRingB{};
    check(buildTargetRing(box, camera, false, 48.0f, 1.25, staticRingA),
          "static target ring geometry generated");
    check(buildTargetRing(box, camera, false, 48.0f, 9.5, staticRingB),
          "static target ring geometry generated for a second timestamp");
    check(buildTargetRing(box, camera, true, 48.0f, 1.25, animatedRingA),
          "animated target ring geometry generated");
    check(buildTargetRing(box, camera, true, 48.0f, 9.5, animatedRingB),
          "animated target ring geometry generated at a second timestamp");
    check(staticRingA.outer.size() == 60 && staticRingA.inner.size() == 68,
          "ring segment counts stay fixed and allocation-free");
    check(same(staticRingA.outer[0].first, staticRingB.outer[0].first),
          "disabled animation is independent of wall-clock time");
    check(!same(animatedRingA.outer[0].first, animatedRingB.outer[0].first),
          "enabled animation changes outer ring phase");

    const Vec3 ringCenter{
        (box.min.x + box.max.x) * 0.5f,
        box.max.y + std::fmax(0.18f, (box.max.y - box.min.y) * 0.14f),
        (box.min.z + box.max.z) * 0.5f
    };
    Vec3 viewNormal{camera.x-ringCenter.x, camera.y-ringCenter.y, camera.z-ringCenter.z};
    const float normalLength = std::sqrt(viewNormal.x*viewNormal.x +
                                         viewNormal.y*viewNormal.y +
                                         viewNormal.z*viewNormal.z);
    viewNormal.x /= normalLength; viewNormal.y /= normalLength; viewNormal.z /= normalLength;
    const Vec3 radial{animatedRingA.outer[0].first.x-ringCenter.x,
                      animatedRingA.outer[0].first.y-ringCenter.y,
                      animatedRingA.outer[0].first.z-ringCenter.z};
    check(std::fabs(radial.x*viewNormal.x + radial.y*viewNormal.y + radial.z*viewNormal.z) < 0.01f,
          "reticle geometry lies in a camera-facing plane");
    for (const auto& segment : animatedRingA.outer) {
        check(finite(segment.first) && finite(segment.second), "outer ring endpoints are finite");
    }
    for (const auto& segment : animatedRingA.inner) {
        check(finite(segment.first) && finite(segment.second), "inner ring endpoints are finite");
    }
    check(!buildTargetRing(box, camera, true, 48.0f, NAN, animatedRingA),
          "non-finite animation time rejected");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
