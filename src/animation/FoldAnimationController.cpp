// ---------------------------------------------------------------------------
//  FoldAnimationController.cpp
// ---------------------------------------------------------------------------
#include "FoldAnimationController.h"

#include <algorithm>
#include <cmath>

namespace dragonfly {

namespace {

double Clamp(double value, double low, double high) {
    return std::min(std::max(value, low), high);
}

} // namespace

void FoldAnimationController::Configure(double smoothingTauSeconds) {
    m_tau = Clamp(smoothingTauSeconds, 0.001, 1.0);
}

void FoldAnimationController::Configure(double smoothingTauSeconds,
                                        double snapThresholdProgress) {
    Configure(smoothingTauSeconds);
    m_snapThreshold = Clamp(snapThresholdProgress, 0.02, 1.0);
}

void FoldAnimationController::Reset(float progress) {
    m_progress = Clamp(progress, 0.0, 1.0);
    m_velocity = 0.0;
    m_moving = false;
}

void FoldAnimationController::Update(const HingeState& hinge, double dtSeconds) {
    double dt = dtSeconds;
    if (!(dt > 0.0) || dt > 0.25) {
        dt = 1.0 / 60.0;
    }

    const double target = Clamp(hinge.foldProgress, 0.0, 1.0);
    const double jump = std::abs(target - m_progress);

    if (jump > m_snapThreshold) {
        // Large discontinuity: cold-start anchoring, a reset, or entering
        // manual mode.  Sweeping towards it would report a huge fake velocity,
        // so snap instead.
        m_progress = target;
        m_velocity = 0.0;
    } else if (hinge.manual) {
        // Manual control is the operator's own motion: track it exactly, but
        // still report a velocity so the effect can react to it.
        const double instant = (target - m_progress) / dt;
        m_progress = target;
        m_velocity += (instant - m_velocity) * 0.4;
    } else {
        const double previous = m_progress;
        const double alpha = 1.0 - std::exp(-dt / std::max(0.001, m_tau));
        m_progress += (target - m_progress) * alpha;
        const double instant = (m_progress - previous) / dt;
        // Light smoothing keeps the reported velocity readable without adding
        // noticeable lag to the effect.
        m_velocity += (instant - m_velocity) * 0.35;
    }

    m_progress = Clamp(m_progress, 0.0, 1.0);
    m_confidence = hinge.confidence;
    m_manual = hinge.manual;
    m_moving = std::abs(m_velocity) > 0.02;
}

FoldAnimationState FoldAnimationController::State() const {
    FoldAnimationState state;
    state.progress = static_cast<float>(m_progress);
    state.velocity = static_cast<float>(m_velocity);
    state.opening = m_velocity > 0.0;
    state.moving = m_moving;
    state.confidence = m_confidence;
    state.manual = m_manual;
    return state;
}

} // namespace dragonfly
