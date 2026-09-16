// ---------------------------------------------------------------------------
//  FoldAnimationController.h
//
//  Sits between the estimator and the renderer.
//
//  The estimator publishes a progress value that can step slightly when a
//  lid-mode anchor pulls it or when the gyro sign is re-resolved.  The renderer
//  must never see those steps, so this controller low-passes the progress and
//  derives a stable velocity from the smoothed value.
//
//  Contract: the renderer only ever consumes FoldAnimationState.  It never
//  reads the sensor layer.
// ---------------------------------------------------------------------------
#pragma once

#include "../hinge/HingeTypes.h"

namespace dragonfly {

struct FoldAnimationState {
    float progress = 0.0f;     // 0..1, smoothed
    float velocity = 0.0f;     // progress units per second
    bool opening = false;      // direction of travel
    bool moving = false;       // true while |velocity| is significant
    float confidence = 0.0f;   // carried through from the estimator
    bool manual = false;       // progress is operator-driven
};

class FoldAnimationController {
public:
    void Configure(double smoothingTauSeconds);
    void Configure(double smoothingTauSeconds, double snapThresholdProgress);

    // Snaps to a value without animating (used on reset / manual mode entry).
    void Reset(float progress);

    void Update(const HingeState& hinge, double dtSeconds);

    FoldAnimationState State() const;

private:
    double m_progress = 0.0;
    double m_velocity = 0.0;
    double m_tau = 0.05;
    double m_snapThreshold = 0.25;
    float m_confidence = 0.0f;
    bool m_moving = false;
    bool m_manual = false;
};

} // namespace dragonfly
