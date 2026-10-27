#pragma once

#include <algorithm>
#include <cmath>

// Audio-frame timing, independent of browser repaint/polling and track transport.
class VinylBrowseSignalLoss {
  public:
    bool update(double seconds, bool preservingBrowse, bool motionSignal) {
        if (!preservingBrowse || motionSignal || !std::isfinite(seconds) ||
                seconds <= 0 || seconds > 1) {
            m_quietSeconds = 0;
            return false;
        }
        m_quietSeconds = std::min(kQuietSeconds, m_quietSeconds + seconds);
        return m_quietSeconds >= kQuietSeconds;
    }

  private:
    static constexpr double kQuietSeconds = 0.2;
    double m_quietSeconds = 0;
};
