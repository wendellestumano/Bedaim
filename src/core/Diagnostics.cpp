#include "Diagnostics.hpp"
#include "../config/AtomicConfigWriter.hpp"

#include <deque>
#include <unordered_map>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <exception>
#include <system_error>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace bedrocktools::diagnostics {
namespace {
constexpr std::size_t kCapacity = 4096;
std::mutex gMutex;
std::deque<Entry> gEntries;
std::uint64_t gNextSequence = 1;
std::mutex gProbeMutex;
std::unordered_map<std::string, std::string> gProbeStates;

std::int64_t nowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string safeField(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else out.push_back(c);
    }
    return out;
}
}

void probe(Level level, std::string_view component, std::string_view key,
           std::string_view state, std::string_view detail) noexcept {
    try {
        std::string composite;
        composite.reserve(component.size() + key.size() + 1);
        composite.append(component);
        composite.push_back('\x1f');
        composite.append(key);

        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(gProbeMutex);
            const auto it = gProbeStates.find(composite);
            if (it == gProbeStates.end() || it->second != state) {
                gProbeStates[composite] = std::string(state);
                changed = true;
            }
        }
        if (!changed) return;

        std::string message;
        message.reserve(key.size() + state.size() + detail.size() + 4);
        message.append(key);
        message.push_back('=');
        message.append(state);
        if (!detail.empty()) {
            message.append(": ");
            message.append(detail);
        }
        log(level, component, message);
    } catch (...) {
        // Probe bookkeeping is best-effort and must never affect game execution.
    }
}

void resetProbes() noexcept {
    try {
        std::lock_guard<std::mutex> lock(gProbeMutex);
        gProbeStates.clear();
    } catch (...) {
    }
}

void log(Level level, std::string_view component, std::string_view message) noexcept {
    try {
        Entry entry;
        entry.timestampMs = nowMs();
        entry.level = level;
        entry.component = safeField(component);
        entry.message = safeField(message);
        {
            std::lock_guard<std::mutex> lock(gMutex);
            entry.sequence = gNextSequence++;
            if (gEntries.size() == kCapacity) gEntries.pop_front();
            gEntries.push_back(std::move(entry));
        }
    } catch (...) {
        // Diagnostics must never crash or interrupt the game because memory is low.
    }
}

std::vector<Entry> snapshot() {
    std::lock_guard<std::mutex> lock(gMutex);
    return std::vector<Entry>(gEntries.begin(), gEntries.end());
}

std::size_t count() noexcept {
    try {
        std::lock_guard<std::mutex> lock(gMutex);
        return gEntries.size();
    } catch (...) {
        return 0;
    }
}

void clear() noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gEntries.clear();
            // A cleared buffer starts a new sequence so exports are easier to
            // distinguish and correlate within a single gameplay session.
            gNextSequence = 1;
        }
        resetProbes();
    } catch (...) {
    }
}

std::string levelName(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warning: return "WARN";
        case Level::Error: return "ERROR";
        case Level::Critical: return "CRITICAL";
    }
    return "UNKNOWN";
}

bool exportJsonTo(const std::string& path, std::string* error) noexcept {
    try {
        if (path.empty()) {
            if (error) *error = "empty export path";
            return false;
        }

        const auto entries = snapshot();
        nlohmann::json payload;
        payload["schema_version"] = 1;
        payload["generated_at_unix_ms"] = nowMs();
        payload["entry_count"] = entries.size();
        payload["buffer_capacity"] = kCapacity;
        payload["entries"] = nlohmann::json::array();
        for (const auto& entry : entries) {
            payload["entries"].push_back({
                {"timestamp_ms", entry.timestampMs},
                {"sequence", entry.sequence},
                {"level", levelName(entry.level)},
                {"component", entry.component},
                {"message", entry.message}
            });
        }

        if (!bedrocktools::config::writePayloadAtomically(path, payload.dump(2) + "\n")) {
            if (error) *error = "could not atomically replace exported JSON log";
            return false;
        }
        if (error) error->clear();
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    } catch (...) {
        if (error) *error = "unknown JSON export failure";
        return false;
    }
}

bool exportTo(const std::string& path, std::string* error) noexcept {
    try {
        if (path.empty()) {
            if (error) *error = "empty export path";
            return false;
        }
        const auto entries = snapshot();
        std::ostringstream payload;
        payload << "# Bedaim diagnostics export\n";
        payload << "# entries=" << entries.size() << " capacity=" << kCapacity << "\n";
        payload << "# Fields: timestamp_ms | sequence | level | component | message\n";
        for (const auto& entry : entries) {
            payload << entry.timestampMs << " | " << entry.sequence << " | "
                    << levelName(entry.level) << " | " << entry.component << " | "
                    << entry.message << '\n';
        }

        // Reuse the config writer's safe same-filesystem replacement. If rename
        // fails, the old report remains intact; diagnostics must not destroy the
        // previous export while trying to write a new one.
        if (!bedrocktools::config::writePayloadAtomically(path, payload.str())) {
            if (error) *error = "could not atomically replace exported log";
            return false;
        }
        if (error) error->clear();
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    } catch (...) {
        if (error) *error = "unknown export failure";
        return false;
    }
}

} // namespace bedrocktools::diagnostics
