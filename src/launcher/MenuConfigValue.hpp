#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace bedaim::menuconfig {

enum class ApplyResult {
    Applied,
    InvalidValue
};

// Applies a menu payload to one JSON key without mutating unrelated fields.
// String-like and missing/null keys accept the raw payload as a string.
ApplyResult applyValue(nlohmann::json& config,
                       std::string_view key,
                       std::string_view rawValue);

} // namespace bedaim::menuconfig
