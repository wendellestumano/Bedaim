#include "modules/player/AimAssistMath.hpp"
#include "modules/player/AimAssistPolicy.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
int failures = 0;
int checks = 0;

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}
}

int main() {
    using bedaim::aimmath::Bounds;
    using bedaim::aimmath::Vec3;
    using bedaim::aimmath::boundedTurnStep;
    using bedaim::aimmath::isFiniteVector;
    using bedaim::aimmath::isValidBounds;
    using bedaim::aimmath::wrapDegrees;
    using bedaim::aimmath::aimPointAtHeight;
    using bedaim::aimmath::fourAimAnchors;
    using bedaim::aimmath::nearestAnchorToRay;
    using bedaim::aimpolicy::HeldWeapon;
    using bedaim::aimpolicy::isRangedPredictionWeapon;
    check(bedaim::aimpolicy::shouldUseStableAimAnchor(true),
          "Sticky tracking uses the stable hitbox anchor");
    check(!bedaim::aimpolicy::shouldUseStableAimAnchor(false),
          "Smooth and Adaptive tracking retain the sliding hitbox point");

    const float wrapAcrossBoundary = wrapDegrees(-179.0f - 179.0f);
    check(std::abs(wrapAcrossBoundary - 2.0f) < 1.0e-4f,
          "yaw crossing +179 to -179 uses the short +2 degree turn");
    check(std::abs(wrapDegrees(181.0f) + 179.0f) < 1.0e-4f,
          "angles above +180 wrap into the canonical interval");
    check(std::abs(wrapDegrees(-181.0f) - 179.0f) < 1.0e-4f,
          "angles below -180 wrap into the canonical interval");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    check(!isFiniteVector(Vec3{nan, 0.0f, 0.0f}),
          "non-finite vector components are rejected");
    check(!isFiniteVector(Vec3{0.0f, inf, 0.0f}),
          "infinite vector components are rejected");

    check(isValidBounds(Bounds{{-0.3f, 0.0f, -0.3f}, {0.3f, 1.8f, 0.3f}}),
          "ordinary actor bounds are accepted");
    check(!isValidBounds(Bounds{{0.3f, 0.0f, -0.3f}, {-0.3f, 1.8f, 0.3f}}),
          "inverted x bounds are rejected");
    check(!isValidBounds(Bounds{{-0.3f, 1.0f, -0.3f}, {0.3f, 1.0f, 0.3f}}),
          "zero-height bounds are rejected");
    check(!isValidBounds(Bounds{{-0.3f, 0.0f, -0.3f}, {nan, 1.8f, 0.3f}}),
          "bounds with non-finite coordinates are rejected");

    const Bounds aimBounds{{-0.3f, 0.0f, -0.3f}, {0.3f, 2.0f, 0.3f}};
    const Vec3 midAim = aimPointAtHeight(aimBounds, 0.55f);
    const Vec3 highAim = aimPointAtHeight(aimBounds, 0.80f);
    check(std::abs(midAim.y - 1.10f) < 1.0e-4f,
          "weapon Y profile 0.55 maps to 55 percent of native hitbox height");
    check(std::abs(highAim.y - 1.60f) < 1.0e-4f && highAim.y > midAim.y,
          "weapon Y profile 0.80 aims higher without changing hitbox dimensions");

    const auto highAnchors = fourAimAnchors(aimBounds, 0.80f);
    check(highAnchors[0].y == highAim.y && highAnchors[1].y == highAim.y &&
          highAnchors[2].y == highAim.y && highAnchors[3].y == highAim.y,
          "all four anchors retain the weapon's configured vertical aim point");
    check(highAnchors[0].x < highAim.x && highAnchors[1].x > highAim.x &&
          highAnchors[2].z < highAim.z && highAnchors[3].z > highAim.z,
          "four anchors occupy left, right, back, and front positions inside the hitbox");
    const std::size_t nearestSideAnchor = nearestAnchorToRay(
        Vec3{-2.0f, 1.60f, 0.20f}, Vec3{1.0f, 0.0f, 0.0f}, highAnchors);
    check(nearestSideAnchor == 3,
          "four-point aiming picks the anchor nearest the forward view ray, not the box-center projection");

    const float turn = boundedTurnStep(90.0f, 120.0f, 1.0f / 60.0f);
    check(std::isfinite(turn) && turn <= 2.0001f && turn > 0.0f,
          "turn step never exceeds turn rate times frame delta");
    check(std::abs(boundedTurnStep(-90.0f, 120.0f, 1.0f / 60.0f) + turn) < 1.0e-4f,
          "negative turn direction is symmetric");
    check(std::abs(boundedTurnStep(90.0f, 120.0f, 1.0f) - 6.0f) < 1.0e-4f,
          "frame delta is clamped to at most 50 milliseconds");
    check(boundedTurnStep(nan, 120.0f, 1.0f / 60.0f) == 0.0f,
          "non-finite angular error produces no camera delta");
    check(boundedTurnStep(90.0f, nan, 1.0f / 60.0f) == 0.0f,
          "non-finite turn rate produces no camera delta");

    check(isRangedPredictionWeapon(HeldWeapon::Bow),
          "bow uses ranged target-motion prediction");
    check(isRangedPredictionWeapon(HeldWeapon::Crossbow),
          "crossbow uses ranged target-motion prediction");
    check(isRangedPredictionWeapon(HeldWeapon::Trident),
          "trident retains ranged/throwable target-motion prediction");
    check(!isRangedPredictionWeapon(HeldWeapon::Spear),
          "sample-defined piercing/kinetic spears are not treated as projectile weapons");
    check(!isRangedPredictionWeapon(HeldWeapon::Sword) &&
          !isRangedPredictionWeapon(HeldWeapon::Axe) &&
          !isRangedPredictionWeapon(HeldWeapon::Mace) &&
          !isRangedPredictionWeapon(HeldWeapon::OtherMelee),
          "melee weapon classes do not use ranged-only prediction");
    check(!isRangedPredictionWeapon(HeldWeapon::Tool),
          "tool category stays distinct and does not inherit other-melee prediction");

    using bedaim::aimpolicy::classifyHeldWeaponIdentifier;
    check(classifyHeldWeaponIdentifier("minecraft:diamond_sword") == HeldWeapon::Sword,
          "vanilla sword item identifier selects Sword");
    check(classifyHeldWeaponIdentifier("minecraft:crossbow") == HeldWeapon::Crossbow,
          "Crossbow is classified before the generic Bow substring");
    check(classifyHeldWeaponIdentifier("minecraft:diamond_pickaxe") == HeldWeapon::Tool,
          "Pickaxe is classified as a tool before generic Axe matching");
    check(classifyHeldWeaponIdentifier("addon:war_axe") == HeldWeapon::Axe,
          "custom axe item identifier maps to Axe");
    check(classifyHeldWeaponIdentifier("addon:katana") == HeldWeapon::OtherMelee,
          "known custom melee identifiers retain OtherMelee classification");
    check(classifyHeldWeaponIdentifier("addon:war_spear") == HeldWeapon::Spear,
          "spear identifier maps to the spear category");
    check(classifyHeldWeaponIdentifier("") == HeldWeapon::None,
          "empty item identifier fails closed");
    const float nearScale = bedaim::aimmath::distanceHitboxScale(1.0f, 0.5f, 30.0f, 5.0f, true);
    const float mediumScale = bedaim::aimmath::distanceHitboxScale(1.0f, 10.0f, 30.0f, 5.0f, true);
    const float farScale = bedaim::aimmath::distanceHitboxScale(1.0f, 30.0f, 30.0f, 5.0f, true);
    check(nearScale < 1.0f, "distance hitbox is smaller than native near the player");
    check(mediumScale > nearScale && mediumScale < farScale,
          "distance hitbox grows smoothly between near and far ranges");
    check(farScale > 2.0f && farScale < 4.0f,
          "default distance strength provides visible far-range expansion");
    const float normalRangeFarScale =
        bedaim::aimmath::distanceHitboxScale(1.0f, 5.5f, 5.5f, 5.0f, true);
    check(normalRangeFarScale > 2.0f && normalRangeFarScale < 4.0f,
          "default strength still expands at normal 5.5-block aim range");
    check(std::abs(bedaim::aimmath::distanceHitboxScale(2.0f, 30.0f, 30.0f, 5.0f, false) - 2.0f) < 1.0e-4f,
          "disabled distance curve uses the base hitbox scale");
    check(std::isfinite(bedaim::aimmath::distanceHitboxScale(1.0f,
              std::numeric_limits<float>::infinity(), 80.0f, 200.0f, true)),
          "non-finite distance does not poison the hitbox scale");
    const float maxStrengthScale =
        bedaim::aimmath::distanceHitboxScale(1.0f, 30.0f, 30.0f, 500.0f, true);
    check(std::isfinite(maxStrengthScale) && maxStrengthScale > 12.0f && maxStrengthScale <= 40.0f,
          "distance hitbox strength accepts 500 and produces a larger bounded steering volume");
    check(bedaim::aimmath::distanceHitboxScale(1.0f, 80.0f, 80.0f, 500.0f, true) <= 40.0f,
          "far-range hitbox multiplier remains bounded at maximum strength");
    const auto strengthLimits = bedaim::aimmath::normalizeDistanceStrengthLimits(2.0f, 20.0f);
    check(std::abs(strengthLimits.minimum - 2.0f) < 1.0e-4f &&
          std::abs(strengthLimits.maximum - 20.0f) < 1.0e-4f,
          "strength limits preserve a valid configured interval");
    const auto repairedLimits = bedaim::aimmath::normalizeDistanceStrengthLimits(25.0f, 10.0f);
    check(std::abs(repairedLimits.minimum - 25.0f) < 1.0e-4f &&
          std::abs(repairedLimits.maximum - 25.0f) < 1.0e-4f,
          "strength limits repair an inverted interval without leaving min above max");
    check(std::abs(bedaim::aimmath::clampDistanceHitboxStrength(1.0f, 2.0f, 20.0f) - 2.0f) < 1.0e-4f,
          "distance strength respects the configured minimum");
    check(std::abs(bedaim::aimmath::clampDistanceHitboxStrength(30.0f, 2.0f, 20.0f) - 20.0f) < 1.0e-4f,
          "distance strength respects the configured maximum");
    check(std::abs(bedaim::aimmath::clampDistanceHitboxStrength(10.0f, 2.0f, 20.0f) - 10.0f) < 1.0e-4f,
          "distance strength stays unchanged inside configured limits");

    std::cout << checks << " checks, " << failures << " failures\\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
