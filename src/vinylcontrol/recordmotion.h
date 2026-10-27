#pragma once

#include <algorithm>
#include <cmath>

// A short lease on fresh decoder motion, independent of track play/volume.
// Expire on stationary/silent input or a stalled decoder; disable immediately.
class RecordMotion {
  public:
    void update(bool enabled, double frame, bool moving, double seconds) {
        if (!enabled || !std::isfinite(frame)) { reset(); return; }
        const bool fresh = frame != m_frame;
        m_frame = frame;
        if (fresh && moving) { m_active = true; m_quiet = 0; }
        else {
            m_quiet += std::max(0.0, seconds);
            if (m_quiet >= 0.2) m_active = false;
        }
    }
    bool active() const { return m_active; }
    void reset() { m_frame = -1; m_active = false; m_quiet = 0; }
  private:
    double m_frame = -1, m_quiet = 0;
    bool m_active = false;
};
