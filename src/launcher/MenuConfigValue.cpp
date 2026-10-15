#include "MenuConfigValue.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <system_error>
#include <algorithm>

namespace bedaim::menuconfig {
namespace {

bool parseBoolean(std::string_view raw, bool& value) {
    std::string normalized(raw);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (normalized == "true" || normalized == "1" ||
        normalized == "on" || normalized == "yes") {
        value = true;
        return true;
    }
    if (normalized == "false" || normalized == "0" ||
        normalized == "off" || normalized == "no") {
        value = false;
        return true;
    }
    return false;
}

template <typename Integer>
bool parseInteger(std::string_view raw, Integer& value) {
    if (raw.empty()) return false;
    const char* first = raw.data();
    const char* last = first + raw.size();
    const auto result = std::from_chars(first, last, value, 10);
    return result.ec == std::errc{} && result.ptr == last;
}

bool parseFloat(std::string_view raw, double& value) {
    if (raw.empty()) return false;
    // strtod accepts leading whitespace; menu payloads must be exact, not forgiving.
    if (std::isspace(static_cast<unsigned char>(raw.front())) ||
        std::isspace(static_cast<unsigned char>(raw.back()))) {
        return false;
    }

    const std::string text(raw);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !end || *end != '\\0' ||
        errno == ERANGE || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

} // namespace

ApplyResult applyValue(nlohmann::json& config,
                       std::string_view key,
                       std::string_view rawValue) {
    const std::string ownedKey(key);
    const auto it = config.find(ownedKey);
    if (it == config.end() || it->is_null() || it->is_string()) {
        config[ownedKey] = std::string(rawValue);
        return ApplyResult::Applied;
    }

    if (it->is_boolean()) {
        bool parsed = false;
        if (!parseBoolean(rawValue, parsed)) return ApplyResult::InvalidValue;
        config[ownedKey] = parsed;
        return ApplyResult::Applied;
    }

    if (it->is_number_unsigned()) {
        std::uint64_t parsed = 0;
        if (!parseInteger(rawValue, parsed)) return ApplyResult::InvalidValue;
        config[ownedKey] = parsed;
        return ApplyResult::Applied;
    }

    if (it->is_number_integer()) {
        std::int64_t parsed = 0;
        if (!parseInteger(rawValue, parsed)) return ApplyResult::InvalidValue;
        config[ownedKey] = parsed;
        return ApplyResult::Applied;
    }

    if (it->is_number_float()) {
        double parsed = 0.0;
        if (!parseFloat(rawValue, parsed)) return ApplyResult::InvalidValue;
        config[ownedKey] = parsed;
        return ApplyResult::Applied;
    }

    // Preserve the existing menu behavior for non-scalar fields, while keeping
    // the mutation restricted to the requested key.
    config[ownedKey] = std::string(rawValue);
    return ApplyResult::Applied;
}

} // namespace bedaim::menuconfig
