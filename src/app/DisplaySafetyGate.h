// ---------------------------------------------------------------------------
//  DisplaySafetyGate.h
//
//  Fail closed, then wait for a stable environment before restarting capture.
//
//  Strategy from lid-plane (jh3y/lid-plane, GPL-3.0-or-later), reimplemented
//  for the Windows power/display APIs.  Its README sums up the failure this
//  guards against: an overlay left on screen while the panel is off shows a
//  snapshot that has nothing to do with the desktop behind it.  Concretely, a
//  lid that shuts puts the angle delta at its maximum, so the effect would sit
//  there at full blur -- frozen -- for as long as the lid stays closed, and the
//  first thing seen when the panel comes back is that stale frame.
//
//  So: while the lid is shut, the display is unavailable or the sensor has
//  stopped delivering, the effect is paused AND its overlay removed.  Coming
//  out of that state the gate keeps the effect hidden until every input has
//  been healthy for `recoveryDelay` seconds, so a panel that is still waking up
//  (or a dock that is still switching modes) cannot be covered by a frame
//  captured mid-transition.
// ---------------------------------------------------------------------------
#pragma once

namespace dragonfly {

class DisplaySafetyGate {
public:
    enum class State {
        Closed,       // the lid is shut; there is nothing to draw on
        DisplayOff,   // the monitor is off or the output is not usable
        CaptureLost,  // the duplication is gone (mode change, secure desktop)
        SensorStale,  // no sensor readings for too long to trust the angle
        Recovering,   // healthy again, waiting for the environment to settle
        Ready,
    };

    // Fails closed: the next Update() starts a fresh settling period.
    void Reset();

    void SetRecoveryDelay(double seconds) { m_recoveryDelay = seconds; }
    double RecoveryDelay() const { return m_recoveryDelay; }

    State Current() const { return m_state; }
    bool Ready() const { return m_state == State::Ready; }

    // Human-readable form for the status line.
    static const char* Text(State state);

    // True only once every input has been healthy continuously for
    // `recoveryDelay` seconds.  Any unhealthy input both fails and re-arms the
    // timer, so the caller can hide its overlay whenever this returns false.
    //
    // `now` is on the same monotonic timeline as the sensor stamps
    // (SteadySeconds()).
    bool Update(bool lidClosed, bool displayUsable, bool captureHealthy,
                bool sensorFresh, double now);

private:
    State m_state = State::Recovering;
    double m_recoveryDelay = 0.5;
    double m_healthySince = -1.0;
};

} // namespace dragonfly
