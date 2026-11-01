#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace bedaim::macefall::actor_flag {

inline constexpr std::uintptr_t kReaderRva = 0xFD8CAD8u;
inline constexpr std::uintptr_t kHelperRva = 0x11F094B0u;
inline constexpr std::uint32_t kActorDataFlagComponentId = 0xC67426F3u;
inline constexpr int kGlidingFlagBit = 32;

constexpr std::uint32_t fnv1a32(std::string_view value) noexcept {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

inline constexpr std::array<std::uint32_t, 2> kReaderInstructions{
    0x91002000u, // add x0, x0, #8
    0x1485F275u  // b 0x11F094B0
};

inline constexpr std::array<std::uint32_t, 4> kHelperInstructions{
    0xF9400409u, // ldr x9, [x0, #8]
    0x5284DE6Au, // mov w10, #0x26f3
    0x72B8CE8Au, // movk w10, #0xc674, lsl #16
    0xA943A12Bu  // ldp x11, x8, [x9, #0x38]
};

constexpr std::uintptr_t decodeAarch64BranchTarget(
    std::uintptr_t instructionAddress, std::uint32_t instruction) noexcept {
    if ((instruction & 0xFC000000u) != 0x14000000u) return 0;

    const std::int64_t immediate = static_cast<std::int64_t>(instruction & 0x03FFFFFFu);
    const std::int64_t signedImmediate =
        (immediate & (std::int64_t{1} << 25)) != 0
            ? immediate - (std::int64_t{1} << 26)
            : immediate;
    const std::int64_t displacement = signedImmediate * 4;

    if (displacement >= 0) {
        const auto amount = static_cast<std::uintptr_t>(displacement);
        if (amount > std::numeric_limits<std::uintptr_t>::max() - instructionAddress)
            return 0;
        return instructionAddress + amount;
    }

    const auto amount = static_cast<std::uintptr_t>(-displacement);
    if (amount > instructionAddress) return 0;
    return instructionAddress - amount;
}

constexpr bool matchesReaderStub(
    const std::array<std::uint32_t, 2>& instructions) noexcept {
    return instructions == kReaderInstructions;
}

constexpr bool readerBranchesToHelper(
    const std::array<std::uint32_t, 2>& instructions) noexcept {
    return decodeAarch64BranchTarget(kReaderRva + sizeof(std::uint32_t),
                                     instructions[1]) == kHelperRva;
}

constexpr bool matchesHelperPrefix(
    const std::array<std::uint32_t, 4>& instructions) noexcept {
    return instructions == kHelperInstructions;
}

static_assert(kActorDataFlagComponentId == 0xC67426F3u);
static_assert(fnv1a32("ActorDataFlagComponent") == kActorDataFlagComponentId);
static_assert(kGlidingFlagBit == 32);
static_assert(readerBranchesToHelper(kReaderInstructions));

} // namespace bedaim::macefall::actor_flag
