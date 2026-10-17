#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bedrocktools::diagnostics {

enum class Level : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical
};

struct Entry {
    std::uint64_t sequence{};
    std::int64_t timestampMs{};
    Level level{Level::Info};
    std::string component;
    std::string message;
};

void log(Level level, std::string_view component, std::string_view message) noexcept;
std::vector<Entry> snapshot();
std::size_t count() noexcept;
void clear() noexcept;
bool exportTo(const std::string& path, std::string* error = nullptr) noexcept;
std::string levelName(Level level);

} // namespace bedrocktools::diagnostics
