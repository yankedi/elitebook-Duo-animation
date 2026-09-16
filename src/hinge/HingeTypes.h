// ---------------------------------------------------------------------------
//  HingeTypes.h
//
//  Types for the v1 hinge estimator.
//
//  IMPORTANT - what this estimates:
//
//    foldProgress  0.0 .. 1.0
//
//  is a *visual* fold progress, i.e. "how far through the open/close
//  animation are we", NOT a metrologically accurate hinge angle.
//
//  The hardware available on this machine (one IMU + a discrete 5-state lid
//  sensor, no HingeAngleSensor provider) physically cannot recover the true
//  screen-vs-base angle: a single IMU measures its own attitude w.r.t. the
//  earth, so whole-device rotation is indistinguishable from hinge motion in
//  the general case.  What the renderer needs is a continuous, responsive,
//  visually correct progress value - that is what this module produces.
//
//  `estimatedAngleDegrees` exists only as a reserved slot for a future version
//  and stays empty in v1.  Nothing here fabricates a number.
// ---------------------------------------------------------------------------
#pragma once

#include "../sensors/SensorTypes.h"

#include <cstdint>
#include <optional>
#include <string>

namespace dragonfly {

// --------------------------------------------------------------------------
//  Raw state codes reported by the Intel Lid Mode sensor on this machine.
//
//  These names are deliberately neutral: no public documentation establishes
//  what 1..5 mean physically, so they are treated purely as opaque interval
//  anchors.  They are NOT mapped to degrees anywhere in this codebase.
// --------------------------------------------------------------------------
enum class RawLidMode : uint32_t {
    Unknown = 0,
    State1 = 1,
    State2 = 2,
    State3 = 3,
    State4 = 4,
    State5 = 5,
};

// --------------------------------------------------------------------------
//  Direction of travel of the fold progress.
// --------------------------------------------------------------------------
enum class HingeDirection {
    Stable,
    Opening,
    Closing,
};

// --------------------------------------------------------------------------
//  Coarse region, derived from the lid-mode anchor plus progress.
//  Region names describe *where in the fold* we are, not a physical angle.
// --------------------------------------------------------------------------
enum class HingeRegion {
    Unknown,
    ClosedSide,     // state 1, progress still in the lower part
    Laptop,         // state 1, progress past the lower part
    NearFlat,       // state 2
    BeyondFlat,     // state 3
    TentOrStand,    // state 4
    Tablet,         // state 5
};

// --------------------------------------------------------------------------
//  Which physical gyro axis carries the hinge motion.
//  "Auto" lets the estimator pick it; the explicit values are config overrides.
// --------------------------------------------------------------------------
enum class HingeAxisSetting {
    Auto,
    X,
    Y,
    Z,
    NegX,
    NegY,
    NegZ,
};

const char* ToString(RawLidMode value);
const char* ToString(HingeDirection value);
const char* ToString(HingeRegion value);
const char* ToString(HingeAxisSetting value);

// --------------------------------------------------------------------------
//  Estimator output.
// --------------------------------------------------------------------------
struct HingeState {
    float foldProgress = 0.0f;       // 0.0 .. 1.0, visual fold progress
    float angularVelocity = 0.0f;    // progress units per second (signed)
    float confidence = 0.0f;         // 0.0 .. 1.0

    uint32_t rawLidMode = 0;         // 0 = unavailable

    HingeDirection direction = HingeDirection::Stable;
    HingeRegion region = HingeRegion::Unknown;

    // Reserved for a future, calibrated estimator. Intentionally empty in v1.
    std::optional<float> estimatedAngleDegrees;

    // ---- diagnostics (shown by the debug UI, useful when tuning) ----------
    int axisIndex = -1;              // 0=X 1=Y 2=Z, -1 = not yet selected
    float axisSign = 1.0f;           // +1 / -1 applied to the selected axis
    float axisRateDegPerSec = 0.0f;  // signed rate used for integration
    float axisShare = 0.0f;          // share of the window energy on that axis
    bool axisSignConfident = false;  // true once a lid transition resolved it
    bool wholeDeviceMotion = false;  // suppression active
    bool manual = false;             // manual override is driving progress
    bool sensorsValid = false;       // gyro + orientation present
};

// --------------------------------------------------------------------------
//  Tunables.  Every number lives here and can be overridden from
//  src/config/config.json -- nothing is hard coded at the use sites.
// --------------------------------------------------------------------------
struct HingeEstimatorConfig {
    // "auto" | "x" | "-x" | "y" | "-y" | "z" | "-z"
    std::string hingeAxis = "auto";

    // ---- gyro conditioning ------------------------------------------------
    double gyroDeadZoneDegPerSec = 2.0;
    // progress gained per degree of hinge travel: 1/180 means a full
    // closed-to-flat sweep (180 deg) fills the progress range exactly.
    double gyroProgressGain = 0.0056;

    // Short-term average of the bias-corrected rates.  Sensor noise oscillates
    // around zero and averages out; a slow, deliberate fold does not.  Both the
    // integration and the axis statistics run on this average, which is what
    // separates "the user is slowly opening the lid" from "the gyro is noisy".
    // 0.12 s adds roughly 4 frames of lag at 60 Hz - imperceptible, but enough
    // to reject the noise that would otherwise be integrated into drift.
    double rateSmoothingTauSeconds = 0.12;

    // Median filter applied before the averaging.  This gyro emits isolated
    // large spikes (measured on this machine: X-axis standard deviation
    // 19 deg/s while its mean stays at 0.08 deg/s).  An averaging filter smears
    // such a spike into a persistent false rate that then integrates into
    // drift; a median rejects it outright.  Must be odd; 1 disables it.
    int medianFilterLength = 5;

    // Gyro bias learning.  A stationary gyro still reports a small offset, but
    // bias may ONLY be learned from samples that are unambiguously quiet --
    // otherwise a slow, deliberate fold gets absorbed into the offset and the
    // progress stops following the hardware.  The check uses the per-axis peak,
    // not the vector magnitude, so a fast motion on one axis cannot sneak in.
    double biasLearningRateLimitDegPerSec = 2.5;
    double biasLearningDelaySeconds = 1.5;
    double biasLearningTauSeconds = 3.0;

    // ---- Stable / Opening / Closing state machine -------------------------
    // Units are progress-per-second, not degrees-per-second: with the default
    // gain a 90 deg/s fold produces about 0.5 progress/s, so these thresholds
    // are two orders of magnitude smaller than they look.
    double motionStartThreshold = 0.08;  // ~14 deg/s of hinge motion
    double stableThreshold = 0.02;       // ~3.5 deg/s
    int settleTimeMs = 300;              // how long below stableThreshold

    // ---- lid-mode anchoring ----------------------------------------------
    double state1MaxProgress = 0.90;
    double state2MinProgress = 0.82;
    double state2MaxProgress = 1.00;
    bool state3ForcesOpen = true;

    // Cold-start anchor: with no absolute reference, the first lid-mode read
    // decides where the progress starts, otherwise every launch would begin at
    // 0.0 even when the machine is open.
    double state1DefaultProgress = 0.75;
    double state2DefaultProgress = 0.92;

    // ---- axis sign resolution --------------------------------------------
    // When the lid mode changes, the hinge is definitely moving in a known
    // direction (a rising state code = opening), so the sign of the dominant
    // gyro axis is resolved from the motion that follows the transition.
    double signProbeWindowSeconds = 1.0;
    double signProbeMinRateDegPerSec = 5.0;

    // ---- idle freeze ------------------------------------------------------
    // This gyro's X axis shows a zero-mean noise floor of about 19 deg/s
    // (measured while the machine sat untouched).  Even after the median and
    // averaging filters, the surviving residue occasionally crosses the dead
    // zone, and integrating it over tens of seconds produces a slow random
    // walk.  When nothing above the "real motion" level has been seen for a
    // while, the smoothed rates are deliberately bled to zero so the progress
    // physically cannot drift.
    double idleRateFloorDegPerSec = 8.0;
    double idleFreezeDelaySeconds = 0.8;
    double idleFreezeTauSeconds = 0.25;

    // ---- suppression of whole-device motion -------------------------------
    double orientationCorrectionGain = 0.02;   // smoothing of the suppression
    double accelDeviationLimitG = 0.25;        // |(|a| - 1g)| beyond this = linear motion
    double axisShareFloor = 0.45;              // below this the rotation is not hinge-like
    double motionPenalty = 0.90;               // max fraction of the update suppressed
    // Spread rotation only counts as whole-device motion while the machine is
    // genuinely moving; sensor noise at rest must never trip the suppression.
    double motionConfirmRateDegPerSec = 15.0;

    // ---- auto axis selection ---------------------------------------------
    double axisWindowSeconds = 1.5;
    double axisSwitchRatio = 1.6;
    // Only smoothed rates above this contribute to the axis statistics.  Must
    // match the idle-freeze floor: below it the signal is noise.
    double axisEnergyFloor = 10.0;
    double axisEnergyMinRateDegPerSec = 8.0;

    // ---- manual control ---------------------------------------------------
    double manualStepPerPress = 0.05;

    // ---- sanity -----------------------------------------------------------
    double maxIntegrationDtSeconds = 0.10;
};

} // namespace dragonfly
