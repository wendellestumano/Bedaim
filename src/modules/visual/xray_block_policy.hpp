#pragma once

#include <string_view>

namespace bedaim::xray {

inline std::string_view stripNamespace(std::string_view name) noexcept {
    const std::size_t separator = name.find(':');
    if (separator != std::string_view::npos) name.remove_prefix(separator + 1);
    return name;
}

inline bool isTargetBlockName(std::string_view rawName) noexcept {
    const std::string_view name = stripNamespace(rawName);
    if (name.empty()) return false;

    // Ancient debris does not follow the *_ore naming convention.
    if (name == "ancient_debris") return true;

    // Covers normal, deepslate, nether, and add-on ores with conventional IDs.
    return name.ends_with("_ore");
}

} // namespace bedaim::xray
