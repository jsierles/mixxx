#pragma once

#include <cmath>

// Audio-thread state only: no allocation, wall clock or transport writes.
// A quiet gap must follow decoded timecode and then a stable fresh return.
class NeedleDropDetector {
  public:
    void reset() { *this = {}; }

    bool update(double seconds, bool motionSignal, bool freshPosition,
            double position, bool requirePositionChange = false) {
        if (!std::isfinite(seconds) || seconds <= 0 || seconds > 0.1) {
            reset();
            return false;
        }
        freshPosition = freshPosition && motionSignal && std::isfinite(position);
        if (!motionSignal) {
            m_quiet += seconds;
            m_returnTime = 0;
            if (m_seenPosition && m_quiet >= 0.2) m_lifted = true;
            return false;
        }
        m_quiet = 0;
        if (!freshPosition) {
            // Motion without a position is ordinary during scratching. It is
            // neither a needle lift nor sufficient evidence of a return.
            m_returnTime = 0;
            return false;
        }
        if (m_lifted) {
            if (m_returnTime > 0 &&
                    std::abs(position - m_candidatePosition) > 4 * seconds + 0.025) {
                m_returnTime = 0;
            }
            m_candidatePosition = position;
            m_returnTime += seconds;
            if (m_returnTime < 0.05) return false;
            const bool changed = std::abs(position - m_lastPosition) >= 0.25;
            m_lifted = false;
            m_returnTime = 0;
            m_lastPosition = position;
            return !requirePositionChange || changed;
        }
        m_seenPosition = true;
        m_lastPosition = position;
        return false;
    }

  private:
    double m_quiet = 0;
    double m_returnTime = 0;
    double m_lastPosition = 0;
    double m_candidatePosition = 0;
    bool m_seenPosition = false;
    bool m_lifted = false;
};
