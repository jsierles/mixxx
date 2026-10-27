#pragma once

#include <algorithm>
#include <cmath>
#include <deque>

// UI-thread gesture interpreter. Time is supplied by the caller, making both
// missing input and GUI stalls testable without decks or an audio device.
class VinylBrowseGesture {
  public:
    struct Action { int rows = 0; bool select = false; };
    void reset() { *this = VinylBrowseGesture(); }
    bool armed() const { return m_armed; }
    bool confirming() const { return m_armed && m_stableSeconds > 0; }
    double confirmationRemaining() const { return std::max(0.0, 1.5 - m_stableSeconds); }

    Action update(double speed, bool valid, double seconds, bool canConfirm = true) {
        Action action;
        if (!valid || !std::isfinite(speed) || seconds <= 0 || seconds > 0.1) {
            // A dropout must never complete a release or retain fractional motion.
            m_stableSeconds = m_motionSeconds = m_fraction = 0;
            m_motionDirection = 0;
            return action;
        }
        // Free rotation at the usual pitch-slider settings is neutral. Holding
        // the vinyl stationary is also neutral, but does not mean "release".
        const bool freeRunning = speed >= 0.85 && speed <= 1.15;
        if (freeRunning) {
            m_fraction = m_motionSeconds = 0;
            m_motionDirection = 0;
            if (!canConfirm) { m_stableSeconds = 0; return action; }
            if (m_stableSeconds == 0 || std::abs(speed - m_lastStableSpeed) > 0.01) {
                m_stableSeconds = 0;
                m_lastStableSpeed = speed;
            }
            m_stableSeconds += seconds;
            if (m_armed && m_stableSeconds >= 1.5) {
                action.select = true;
                reset();
            }
            return action;
        }
        m_stableSeconds = 0;
        if (std::abs(speed) < 0.01) {
            m_fraction = m_motionSeconds = 0;
            m_motionDirection = 0;
            return action;
        }
        // Require deliberate motion before release can select anything. Slow
        // forward movement scrolls forward; reverse movement scrolls backward.
        const int direction = speed > 0 ? 1 : -1;
        if (direction != m_motionDirection) {
            m_motionDirection = direction;
            m_motionSeconds = m_fraction = 0;
        }
        const bool firstStep = m_motionSeconds < 0.12;
        m_motionSeconds += seconds;
        if (m_motionSeconds < 0.12) return action;
        m_armed = true;
        // Give slow movement a prompt first step, then a usable minimum rate.
        if (firstStep) { action.rows = direction; return action; }
        const double rate = std::clamp(std::abs(speed) * 8.0, 2.0, 24.0);
        m_fraction += direction * rate * seconds;
        action.rows = static_cast<int>(m_fraction);
        m_fraction -= action.rows;
        return action;
    }

  private:
    bool m_armed = false;
    double m_fraction = 0;
    double m_motionSeconds = 0;
    double m_stableSeconds = 0;
    double m_lastStableSpeed = 1;
    int m_motionDirection = 0;
};

// Explicit-selection browsing: returning to steady platter speed locks the
// highlight; only a deliberate reverse gesture unlocks it. Positive rows wait
// briefly so acceleration into free rotation can be discarded before display.
class VinylBrowseLatch {
  public:
    using Action = VinylBrowseGesture::Action;
    void reset() { *this = VinylBrowseLatch(); }
    bool locked() const { return m_locked; }

    Action update(double speed, bool valid, double seconds) {
        Action action;
        if (!valid || !std::isfinite(speed) || seconds <= 0 || seconds > 0.1) {
            clearMotion();
            m_stableSeconds = 0;
            return action; // Missing signal never unlocks or confirms a release.
        }
        const bool freeRunning = speed >= 0.85 && speed <= 1.15;
        if (freeRunning) {
            clearMotion(); // Drop the release acceleration, including queued rows.
            if (m_stableSeconds == 0 || std::abs(speed - m_stableSpeed) > 0.02) {
                m_stableSeconds = 0;
                m_stableSpeed = speed;
            }
            m_stableSeconds += seconds;
            if (m_stableSeconds >= 0.2) m_locked = true;
            return action;
        }
        m_stableSeconds = 0;
        if (m_locked && speed >= -0.01) {
            clearMotion();
            return action;
        }
        const auto motion = m_motion.update(speed, true, seconds, false);
        if (speed <= 0.01) {
            m_pending.clear();
            m_haveForwardSpeed = false;
            m_acceleration = 0;
            if (motion.rows < 0) {
                m_locked = false;
                action.rows = motion.rows;
            }
            return action;
        }
        if (m_haveForwardSpeed) {
            // Smooth speed noise over about 80 ms. A rise toward platter speed
            // delays commits until the motion settles or reaches free rotation.
            const double alpha = seconds / (0.08 + seconds);
            m_acceleration += alpha * ((speed - m_lastForwardSpeed) / seconds - m_acceleration);
        }
        m_haveForwardSpeed = true;
        m_lastForwardSpeed = speed;
        for (auto& row : m_pending) row.age += seconds;
        if (motion.rows > 0) m_pending.push_back({motion.rows, 0});
        if (speed < 1.15 && m_acceleration > 0.4) return action;
        while (!m_pending.empty() && m_pending.front().age >= 0.25) {
            action.rows += m_pending.front().rows;
            m_pending.pop_front();
        }
        return action;
    }

  private:
    struct Pending { int rows; double age; };
    void clearMotion() {
        m_motion.reset();
        m_pending.clear();
        m_haveForwardSpeed = false;
        m_acceleration = 0;
    }
    VinylBrowseGesture m_motion;
    std::deque<Pending> m_pending;
    bool m_locked = true;
    bool m_haveForwardSpeed = false;
    double m_lastForwardSpeed = 0;
    double m_acceleration = 0;
    double m_stableSeconds = 0;
    double m_stableSpeed = 1;
};
