#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// Changes weights only: every estimator continues receiving the same samples.
// Position/transport authority remains the original timecode decoder.
class PitchAlgorithmSwitch {
  public:
    static bool valid(double value) {
        return std::isfinite(value) && value >= 0 && value <= 2 && value == std::floor(value);
    }
    void reset(int algorithm) {
        m_target = m_active = algorithm;
        m_weights = {{0, 0, 0}};
        m_weights[algorithm] = 1;
        m_from = m_weights;
        m_elapsed = kBlendSeconds;
        m_lastRate = 0;
    }
    void request(int algorithm) {
        if (!valid(algorithm) || algorithm == m_target) return;
        m_from = m_weights;
        m_target = algorithm;
        m_elapsed = 0;
    }
    double process(const std::array<double, 3>& rates, double seconds) {
        for (int i = 0; i < 3; ++i) {
            if ((m_weights[i] > 0 || i == m_target) && !std::isfinite(rates[i])) return m_lastRate;
        }
        if (switching()) {
            m_elapsed = std::min(kBlendSeconds, m_elapsed + std::max(0.0, seconds));
            const double fraction = m_elapsed / kBlendSeconds;
            for (int i = 0; i < 3; ++i) {
                m_weights[i] = m_from[i] * (1 - fraction) + (i == m_target ? fraction : 0);
            }
            if (!switching()) m_active = m_target;
        }
        double result = 0;
        for (int i = 0; i < 3; ++i) {
            if (m_weights[i] > 0) result += rates[i] * m_weights[i];
        }
        if (std::isfinite(result)) m_lastRate = result;
        return m_lastRate;
    }
    int target() const { return m_target; }
    int active() const { return m_active; }
    bool switching() const { return m_elapsed < kBlendSeconds; }

  private:
    static constexpr double kBlendSeconds = .050;
    std::array<double, 3> m_weights{{0, 1, 0}}, m_from{{0, 1, 0}};
    int m_target = 1, m_active = 1;
    double m_elapsed = kBlendSeconds;
    double m_lastRate = 0;
};
