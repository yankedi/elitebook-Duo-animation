// ---------------------------------------------------------------------------
//  HingeEstimator.h
//
//  v1 fold-progress estimator.
//
//  Design summary (see HingeTypes.h for the "why"):
//
//    gyroscope      -> low-latency continuous motion, integrated into progress
//    lid mode       -> discrete anchors: hard progress bounds per state, the
//                      cold-start anchor, and the reference used to resolve the
//                      *sign* of the chosen gyro axis
//    accelerometer  -> gravity reference used to detect linear motion
//    orientation    -> only used to lower confidence / suppress integration
//                      while the whole device is being rotated; it is NEVER
//                      converted into a progress value
//
//  The estimator is deliberately conservative: when the evidence is weak it
//  stops moving rather than inventing motion.
// ---------------------------------------------------------------------------
#pragma once

#include "HingeTypes.h"

#include <mutex>

namespace dragonfly {

class HingeEstimator {
public:
    HingeEstimator();
    ~HingeEstimator();

    HingeEstimator(const HingeEstimator&) = delete;
    HingeEstimator& operator=(const HingeEstimator&) = delete;

    void Configure(const HingeEstimatorConfig& config);
    HingeEstimatorConfig Config() const;

    // Returns the progress to the cold-start anchor for the current lid mode.
    void Reset();

    // Feeds one snapshot. dtSeconds <= 0 falls back to 1/60 s.
    void Update(const SensorSample& sample, double dtSeconds);

    HingeState State() const;

    // ---- manual override --------------------------------------------------
    // Manual mode freezes sensor integration and lets the operator drive the
    // progress directly (keyboard), so the renderer can be exercised even when
    // the sensor path is unavailable or uncalibrated.
    void SetManualEnabled(bool enabled);
    bool ManualEnabled() const;
    void ManualNudge(double delta);
    void ManualSetProgress(double progress);

private:
    void UpdateAxisSelection(double gx, double gy, double gz, double dt);
    void UpdateGyroBias(const double (&values)[3], double dt);
    int PickBestAxis() const;
    void UpdateSignProbe(const double (&values)[3], double nowSeconds);
    void NoteLidMode(uint32_t lidMode, double nowSeconds);
    void ApplyLidAnchor(uint32_t lidMode, double dt);
    void UpdateDirection(double instantVelocity, double dt);
    void UpdateConfidence(const SensorSample& sample, double accelDeviation);
    HingeRegion ComputeRegion(uint32_t lidMode, double progress) const;
    double DefaultProgressForLidMode(uint32_t lidMode) const;
    void CopyStateLocked(HingeState& state) const;

    mutable std::mutex m_mutex;
    HingeEstimatorConfig m_config;

    // ---- progress ---------------------------------------------------------
    double m_progress = 0.0;
    double m_velocity = 0.0;             // smoothed, progress units per second
    bool m_progressInitialised = false;

    // ---- axis selection ---------------------------------------------------
    int m_axisIndex = -1;                // 0/1/2, -1 until the first sample
    double m_axisSign = 1.0;
    bool m_axisSignConfident = false;
    bool m_axisLockedByConfig = false;
    // True while the axis was picked from little evidence; the switch rule is
    // relaxed until one axis is confirmed dominant.
    bool m_axisProvisional = true;
    double m_axisEnergy[3] = {0.0, 0.0, 0.0};
    double m_axisEnergySeconds = 0.0;
    double m_lastAxisShare = 0.0;
    double m_lastWindowEnergy = 0.0;

    // ---- gyro bias --------------------------------------------------------
    double m_gyroBias[3] = {0.0, 0.0, 0.0};
    double m_quietSeconds = 0.0;
    bool m_biasLearned = false;

    // Short-term average of the bias-corrected rates (see config comment).
    double m_smoothedRate[3] = {0.0, 0.0, 0.0};
    double m_idleSeconds = 0.0;

    // Median filter state, one per axis (see config comment).
    static constexpr int kMedianCapacity = 7;
    struct MedianState {
        double buffer[kMedianCapacity] = {};
        int index = 0;
        int length = 5;
        double Push(double value);
    };
    MedianState m_median[3];

    // ---- sign probe -------------------------------------------------------
    bool m_signProbeActive = false;
    bool m_signProbeOpening = true;
    double m_signProbeDeadline = 0.0;

    // ---- lid mode ---------------------------------------------------------
    uint32_t m_lidMode = 0;
    uint32_t m_previousLidMode = 0;
    bool m_hasLidMode = false;

    // ---- state machine ----------------------------------------------------
    HingeDirection m_direction = HingeDirection::Stable;
    double m_stableSeconds = 0.0;

    // ---- whole-device motion ---------------------------------------------
    double m_suppression = 0.0;
    bool m_lastWholeDeviceMotion = false;

    // ---- published outputs (computed during Update) ----------------------
    float m_confidence = 0.0f;
    HingeRegion m_region = HingeRegion::Unknown;

    // ---- diagnostics ------------------------------------------------------
    double m_lastRateDegPerSec = 0.0;
    double m_lastRateMagnitude = 0.0;

    // ---- manual -----------------------------------------------------------
    bool m_manual = false;
    double m_lastManualProgress = 0.0;
    double m_lastManualSeconds = 0.0;
};

} // namespace dragonfly
