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
    std::uint64_t monotonicNs{};
    std::uint64_t threadId{};
    Level level{Level::Info};
    std::string component;
    std::string message;
};

void log(Level level, std::string_view component, std::string_view message) noexcept;
// Emit a log entry only when a named probe changes state; useful for per-tick
// module gates without flooding the bounded log buffer with identical messages.
void probe(Level level, std::string_view component, std::string_view key,
           std::string_view state, std::string_view detail = {}) noexcept;
void resetProbes() noexcept;
std::vector<Entry> snapshot();
// Copy only the newest entries; use for periodic HUD/health reports that display a tail.
std::vector<Entry> snapshotTail(std::size_t maxEntries);
std::size_t count() noexcept;
std::size_t bufferCapacity() noexcept;
std::uint64_t droppedCount() noexcept;
void clear() noexcept;
bool exportTo(const std::string& path, std::string* error = nullptr) noexcept;
bool exportJsonTo(const std::string& path, std::string* error = nullptr) noexcept;
std::string levelName(Level level);

} // namespace bedrocktools::diagnostics
