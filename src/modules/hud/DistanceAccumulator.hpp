#pragma once

#include <algorithm>
#include <cmath>

namespace bedaim::hud {

// Accumulates player displacement without depending on Minecraft SDK types.
class DistanceAccumulator {
public:
    void reset() {
        m_hasLast = false;
        m_lastX = m_lastY = m_lastZ = 0.0f;
        m_totalBlocks = 0.0;
    }

    void update(float x, float y, float z, bool use3D, float maxStepBlocks) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
            std::abs(x) > 30000000.0f || std::abs(y) > 30000000.0f ||
            std::abs(z) > 30000000.0f) {
            m_hasLast = false;
            return;
        }

        if (!m_hasLast) {
            setLast(x, y, z);
            return;
        }

        const float dx = x - m_lastX;
        const float dy = y - m_lastY;
        const float dz = z - m_lastZ;
        const float distanceSquared = use3D
            ? dx * dx + dy * dy + dz * dz
            : dx * dx + dz * dz;
        const float distance = std::sqrt(distanceSquared);
        const float maxStep = std::clamp(maxStepBlocks, 1.0f, 64.0f);

        // Large jumps are treated as teleports/respawns: update the baseline but
        // don't inflate the traveled-distance counter with a discontinuity.
        if (std::isfinite(distance) && distance <= maxStep) {
            m_totalBlocks += static_cast<double>(distance);
        }
        setLast(x, y, z);
    }

    double totalBlocks() const noexcept { return m_totalBlocks; }

private:
    void setLast(float x, float y, float z) {
        m_lastX = x;
        m_lastY = y;
        m_lastZ = z;
        m_hasLast = true;
    }

    bool m_hasLast = false;
    float m_lastX = 0.0f;
    float m_lastY = 0.0f;
    float m_lastZ = 0.0f;
    double m_totalBlocks = 0.0;
};

} // namespace bedaim::hud
