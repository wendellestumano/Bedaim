#pragma once

#include <cstdint>
#include <string_view>

namespace bedrocktools {

inline constexpr std::string_view Name = "Bedaim";
inline constexpr std::string_view Author = "Bedaim Contributors";
inline constexpr std::string_view Description = "A standalone Minecraft Bedrock native module with Bedaim-owned combat and visual features.";
inline constexpr std::string_view Version = "0.1.0";
inline constexpr std::uint32_t SdkVersion = 1;
inline constexpr std::string_view RuntimeLibrary = "libBedaim.so";

}
