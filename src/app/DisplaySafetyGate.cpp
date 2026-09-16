// ---------------------------------------------------------------------------
//  DisplaySafetyGate.cpp
// ---------------------------------------------------------------------------
#include "DisplaySafetyGate.h"

namespace dragonfly {

void DisplaySafetyGate::Reset() {
    m_state = State::Recovering;
    m_healthySince = -1.0;
}

const char* DisplaySafetyGate::Text(State state) {
    switch (state) {
    case State::Closed:
        return "Paused - lid closed";
    case State::DisplayOff:
        return "Paused - display unavailable";
    case State::CaptureLost:
        return "Paused - capture lost";
    case State::SensorStale:
        return "Paused - sensor unavailable";
    case State::Recovering:
        return "Waiting for display...";
    case State::Ready:
        return "Ready";
    }
    return "Unknown";
}

bool DisplaySafetyGate::Update(bool lidClosed, bool displayUsable,
                               bool captureHealthy, bool sensorFresh,
                               double now) {
    // Order matters only for the message: a closed lid explains every other
    // condition, so it is reported first.
    if (lidClosed) {
        m_state = State::Closed;
    } else if (!displayUsable) {
        m_state = State::DisplayOff;
    } else if (!captureHealthy) {
        m_state = State::CaptureLost;
    } else if (!sensorFresh) {
        m_state = State::SensorStale;
    } else {
        if (m_healthySince < 0.0) {
            m_healthySince = now;
        }
        m_state = (now - m_healthySince >= m_recoveryDelay) ? State::Ready
                                                            : State::Recovering;
        return m_state == State::Ready;
    }

    m_healthySince = -1.0;
    return false;
}

} // namespace dragonfly
