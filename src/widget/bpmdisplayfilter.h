#pragma once

#include <algorithm>
#include <cmath>

// Presentation only: callers must never feed this value back to transport.
class BpmDisplayFilter {
  public:
    void reset() { m_initialized = false; }
    double update(double bpm, double elapsedSeconds, bool loaded) {
        if (!loaded || !std::isfinite(bpm) || bpm <= 0) {
            reset();
            return 0;
        }
        // A resumed view or large tempo change should show its new value promptly.
        if (!m_initialized || elapsedSeconds > 2 || std::abs(bpm - m_average) > 2) {
            m_average = bpm;
            m_display = std::round(bpm * 10) / 10;
            m_initialized = true;
            return m_display;
        }
        const double dt = std::isfinite(elapsedSeconds) ? std::max(0.0, elapsedSeconds) : 0;
        m_average += -std::expm1(-dt / 0.75) * (bpm - m_average);
        // One decimal place, with an extra 0.02 BPM beyond the rounding boundary.
        if (std::abs(m_average - m_display) > 0.07) {
            m_display = std::round(m_average * 10) / 10;
        }
        return m_display;
    }

  private:
    bool m_initialized = false;
    double m_average = 0;
    double m_display = 0;
};
