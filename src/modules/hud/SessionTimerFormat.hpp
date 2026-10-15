#pragma once

#include <algorithm>
#include <cstdio>
#include <string>

namespace bedaim::hud {

inline std::string formatSessionDuration(long long totalSeconds, bool showHours = true) {
    totalSeconds = std::max(0LL, totalSeconds);
    const auto hours = totalSeconds / 3600;
    const auto minutes = (totalSeconds / 60) % 60;
    const auto seconds = totalSeconds % 60;

    char buffer[32]{};
    if (showHours || hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld", hours, minutes, seconds);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld", totalSeconds / 60, seconds);
    }
    return buffer;
}

} // namespace bedaim::hud
