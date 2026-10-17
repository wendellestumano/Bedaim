#include "Diagnostics.hpp"

#include <algorithm>
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

namespace bedrocktools::diagnostics {
namespace {
constexpr std::size_t kCapacity = 4096;
std::mutex gMutex;
std::vector<Entry> gEntries;
std::uint64_t gNextSequence = 1;

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
            if (gEntries.size() == kCapacity) gEntries.erase(gEntries.begin());
            gEntries.push_back(std::move(entry));
        }
    } catch (...) {
        // Diagnostics must never crash or interrupt the game because memory is low.
    }
}

std::vector<Entry> snapshot() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gEntries;
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
        std::lock_guard<std::mutex> lock(gMutex);
        gEntries.clear();
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

bool exportTo(const std::string& path, std::string* error) noexcept {
    try {
        if (path.empty()) {
            if (error) *error = "empty export path";
            return false;
        }
        const auto entries = snapshot();
        const std::filesystem::path output(path);
        if (!output.parent_path().empty())
            std::filesystem::create_directories(output.parent_path());

        const std::string temporary = path + ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) {
                if (error) *error = "could not open temporary log file";
                return false;
            }
            file << "# Bedaim diagnostics export\n";
            file << "# entries=" << entries.size() << " capacity=" << kCapacity << "\n";
            file << "# Fields: timestamp_ms | sequence | level | component | message\n";
            for (const auto& entry : entries) {
                file << entry.timestampMs << " | " << entry.sequence << " | "
                     << levelName(entry.level) << " | " << entry.component << " | "
                     << entry.message << '\n';
            }
            file.flush();
            if (!file) {
                if (error) *error = "failed while writing diagnostics";
                file.close();
                std::error_code ec;
                std::filesystem::remove(temporary, ec);
                return false;
            }
        }

        std::error_code ec;
        std::filesystem::rename(temporary, output, ec);
        if (ec) {
            // Android filesystems can reject rename-over-existing; replace safely.
            std::filesystem::remove(output, ec);
            ec.clear();
            std::filesystem::rename(temporary, output, ec);
        }
        if (ec) {
            if (error) *error = "could not finalize exported log: " + ec.message();
            std::filesystem::remove(temporary, ec);
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
