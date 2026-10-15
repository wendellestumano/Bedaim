#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace bedaim::hud {

struct FrameStatsSnapshot {
    std::size_t sampleCount = 0;
    float currentFps = 0.0f;
    float averageFps = 0.0f;
    float onePercentLowFps = 0.0f;
    float averageFrameMs = 0.0f;
    float worstFrameMs = 0.0f;
};

// Pure rolling statistics, independent of Minecraft and Preloader.
class FrameStatsWindow {
public:
    explicit FrameStatsWindow(std::uint64_t windowMs = 5000) : m_windowMs(windowMs) {}

    void setWindowMs(std::uint64_t windowMs) {
        m_windowMs = std::max<std::uint64_t>(1, windowMs);
        prune(m_samples.empty() ? 0 : m_samples.back().timestampMs);
    }

    void reset() { m_samples.clear(); }

    void addSample(std::uint64_t timestampMs, float frameMs) {
        if (!std::isfinite(frameMs) || frameMs < 0.1f || frameMs > 1000.0f) return;
        if (!m_samples.empty() && timestampMs < m_samples.back().timestampMs) m_samples.clear();
        m_samples.push_back({timestampMs, frameMs});
        prune(timestampMs);
        constexpr std::size_t kMaxSamples = 2400;
        if (m_samples.size() > kMaxSamples) {
            m_samples.erase(m_samples.begin(),
                m_samples.begin() + static_cast<std::ptrdiff_t>(m_samples.size() - kMaxSamples));
        }
    }

    FrameStatsSnapshot snapshot() const {
        FrameStatsSnapshot result;
        result.sampleCount = m_samples.size();
        if (m_samples.empty()) return result;

        float totalMs = 0.0f;
        std::vector<float> slowFrames;
        slowFrames.reserve(m_samples.size());
        for (const auto& sample : m_samples) {
            totalMs += sample.frameMs;
            slowFrames.push_back(sample.frameMs);
            result.worstFrameMs = std::max(result.worstFrameMs, sample.frameMs);
        }
        result.averageFrameMs = totalMs / static_cast<float>(m_samples.size());
        result.currentFps = 1000.0f / m_samples.back().frameMs;
        result.averageFps = result.averageFrameMs > 0.0f ? 1000.0f / result.averageFrameMs : 0.0f;

        std::sort(slowFrames.begin(), slowFrames.end(), std::greater<float>());
        const std::size_t slowCount = std::max<std::size_t>(1, (slowFrames.size() + 99) / 100);
        float slowSum = 0.0f;
        for (std::size_t i = 0; i < slowCount; ++i) slowSum += slowFrames[i];
        const float slowAverageMs = slowSum / static_cast<float>(slowCount);
        result.onePercentLowFps = slowAverageMs > 0.0f ? 1000.0f / slowAverageMs : 0.0f;
        return result;
    }

private:
    struct Sample { std::uint64_t timestampMs; float frameMs; };

    void prune(std::uint64_t nowMs) {
        const std::uint64_t cutoff = nowMs > m_windowMs ? nowMs - m_windowMs : 0;
        auto firstKept = m_samples.begin();
        while (firstKept != m_samples.end() && firstKept->timestampMs < cutoff) ++firstKept;
        m_samples.erase(m_samples.begin(), firstKept);
    }

    std::uint64_t m_windowMs;
    std::vector<Sample> m_samples;
};

} // namespace bedaim::hud
