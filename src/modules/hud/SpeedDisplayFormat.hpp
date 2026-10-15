#pragma once

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace bedaim::hud {

inline std::string formatSpeed(float speed, int precision = 0) {
    if (!std::isfinite(speed) || speed < 0.0f) speed = 0.0f;
    precision = std::clamp(precision, 0, 2);

    std::ostringstream text;
    text << "Speed: " << std::fixed << std::setprecision(precision) << speed << " m/s";
    return text.str();
}

} // namespace bedaim::hud
