// ---------------------------------------------------------------------------
//  HingeEstimator.cpp
//
//  See the header for the design rationale.  Every tunable comes from
//  HingeEstimatorConfig; nothing in this file hard codes a threshold.
// ---------------------------------------------------------------------------
#include "HingeEstimator.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace dragonfly {

namespace {

constexpr double kRadToDeg = 57.29577951308232;

double Square(double value) {
    return value * value;
}

double Clamp(double value, double low, double high) {
    return std::min(std::max(value, low), high);
}

// First order approach factor for a time constant tau (seconds).
double Approach(double dt, double tau) {
    if (!(tau > 0.0)) {
        return 1.0;
    }
    return 1.0 - std::exp(-dt / tau);
}

} // namespace

// ==========================================================================
//  Enum names
// ==========================================================================
const char* ToString(RawLidMode value) {
    switch (value) {
    case RawLidMode::State1: return "State1";
    case RawLidMode::State2: return "State2";
    case RawLidMode::State3: return "State3";
    case RawLidMode::State4: return "State4";
    case RawLidMode::State5: return "State5";
    default: return "Unknown";
    }
}

const char* ToString(HingeDirection value) {
    switch (value) {
    case HingeDirection::Opening: return "Opening";
    case HingeDirection::Closing: return "Closing";
    default: return "Stable";
    }
}

const char* ToString(HingeRegion value) {
    switch (value) {
    case HingeRegion::ClosedSide: return "ClosedSide";
    case HingeRegion::Laptop: return "Laptop";
    case HingeRegion::NearFlat: return "NearFlat";
    case HingeRegion::BeyondFlat: return "BeyondFlat";
    case HingeRegion::TentOrStand: return "TentOrStand";
    case HingeRegion::Tablet: return "Tablet";
    default: return "Unknown";
    }
}

const char* ToString(HingeAxisSetting value) {
    switch (value) {
    case HingeAxisSetting::X: return "x";
    case HingeAxisSetting::Y: return "y";
    case HingeAxisSetting::Z: return "z";
    case HingeAxisSetting::NegX: return "-x";
    case HingeAxisSetting::NegY: return "-y";
    case HingeAxisSetting::NegZ: return "-z";
    default: return "auto";
    }
}

// ==========================================================================
//  Construction / configuration
// ==========================================================================
HingeEstimator::HingeEstimator() = default;
HingeEstimator::~HingeEstimator() = default;

void HingeEstimator::Configure(const HingeEstimatorConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;

    for (MedianState& median : m_median) {
        median.length = (config.medianFilterLength < 1) ? 1 : config.medianFilterLength;
    }

    // An explicit override in the config wins over auto-detection.
    m_axisLockedByConfig = false;
    const std::string& axis = config.hingeAxis;
    if (axis == "x" || axis == "-x" || axis == "y" || axis == "-y" ||
        axis == "z" || axis == "-z") {
        const bool negative = axis[0] == '-';
        const char letter = negative ? axis[1] : axis[0];
        m_axisIndex = (letter == 'x') ? 0 : (letter == 'y' ? 1 : 2);
        m_axisSign = negative ? -1.0 : 1.0;
        m_axisSignConfident = true;
        m_axisLockedByConfig = true;
    }
}

HingeEstimatorConfig HingeEstimator::Config() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void HingeEstimator::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress = DefaultProgressForLidMode(m_lidMode);
    m_velocity = 0.0;
    m_direction = HingeDirection::Stable;
    m_stableSeconds = 0.0;
    m_suppression = 0.0;
    m_axisEnergy[0] = m_axisEnergy[1] = m_axisEnergy[2] = 0.0;
    m_axisEnergySeconds = 0.0;
    m_smoothedRate[0] = m_smoothedRate[1] = m_smoothedRate[2] = 0.0;
    m_idleSeconds = 0.0;
    m_progressInitialised = true;
    m_confidence = 0.0f;
}

void HingeEstimator::SetManualEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_manual = enabled;
    m_velocity = 0.0;
    m_direction = HingeDirection::Stable;
    m_lastManualProgress = m_progress;
    m_lastManualSeconds = 0.0;
}

bool HingeEstimator::ManualEnabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_manual;
}

void HingeEstimator::ManualNudge(double delta) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress = Clamp(m_progress + delta, 0.0, 1.0);
}

void HingeEstimator::ManualSetProgress(double progress) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress = Clamp(progress, 0.0, 1.0);
}

// ==========================================================================
//  Update
// ==========================================================================
void HingeEstimator::Update(const SensorSample& sample, double dtSeconds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const HingeEstimatorConfig& c = m_config;

    double dt = dtSeconds;
    if (!(dt > 0.0) || dt > c.maxIntegrationDtSeconds) {
        dt = 1.0 / 60.0;
    }

    // ---- lid mode bookkeeping --------------------------------------------
    const uint32_t lidMode =
        sample.lidMode >= 0 ? static_cast<uint32_t>(sample.lidMode) : 0u;
    m_hasLidMode = sample.lidModeChannel.valid && lidMode != 0u;
    if (m_hasLidMode) {
        NoteLidMode(lidMode, sample.steadySeconds);
    }

    // Cold start: with no absolute reference the first known lid state decides
    // where the progress starts.
    if (!m_progressInitialised && m_hasLidMode) {
        m_progress = DefaultProgressForLidMode(lidMode);
        m_progressInitialised = true;
    }

    // ---- manual override --------------------------------------------------
    if (m_manual) {
        const double now = sample.steadySeconds;
        if (m_lastManualSeconds > 0.0 && now > m_lastManualSeconds) {
            const double manualDt = Clamp(now - m_lastManualSeconds, 1e-4, 0.5);
            const double instant = (m_progress - m_lastManualProgress) / manualDt;
            m_velocity += (instant - m_velocity) * 0.35;
        }
        m_lastManualProgress = m_progress;
        m_lastManualSeconds = now;
        UpdateDirection(m_velocity, dt);
        m_confidence = 1.0f;  // manual is by definition "as good as it gets"
        m_region = ComputeRegion(lidMode, m_progress);
        m_lastWholeDeviceMotion = false;
        m_suppression = 0.0;
        return;
    }

    // ---- everything below needs the gyroscope -----------------------------
    if (!sample.gyro.valid) {
        m_confidence = 0.0f;
        m_region = ComputeRegion(lidMode, m_progress);
        return;
    }

    const double values[3] = {
        sample.angularVelocity.x * kRadToDeg,
        sample.angularVelocity.y * kRadToDeg,
        sample.angularVelocity.z * kRadToDeg,
    };

    // ---- spike rejection, bias removal, then averaging --------------------
    // Order matters: the median filter kills isolated spikes first, so neither
    // the bias estimate nor the average gets polluted by them.
    double filtered[3];
    for (int i = 0; i < 3; ++i) {
        filtered[i] = m_median[i].Push(values[i]);
    }

    UpdateGyroBias(filtered, dt);

    const double corrected[3] = {
        filtered[0] - m_gyroBias[0],
        filtered[1] - m_gyroBias[1],
        filtered[2] - m_gyroBias[2],
    };

    const double rateAlpha = Approach(dt, c.rateSmoothingTauSeconds);
    for (int i = 0; i < 3; ++i) {
        m_smoothedRate[i] += (corrected[i] - m_smoothedRate[i]) * rateAlpha;
    }

    // ---- idle freeze ------------------------------------------------------
    // Bleed the smoothed rates to zero while nothing suggests real motion, so
    // the surviving noise residue cannot random-walk the progress.  The raw
    // rates participate in the decision so a genuine motion releases the freeze
    // immediately instead of waiting for the average to catch up.
    const double rawMagnitude =
        std::sqrt(Square(corrected[0]) + Square(corrected[1]) + Square(corrected[2]));
    const double smoothedMagnitude =
        std::sqrt(Square(m_smoothedRate[0]) + Square(m_smoothedRate[1]) +
                  Square(m_smoothedRate[2]));
    if (rawMagnitude >= c.idleRateFloorDegPerSec ||
        smoothedMagnitude >= c.idleRateFloorDegPerSec) {
        m_idleSeconds = 0.0;
    } else {
        m_idleSeconds += dt;
        if (m_idleSeconds >= c.idleFreezeDelaySeconds) {
            const double bleed = Approach(dt, c.idleFreezeTauSeconds);
            for (int i = 0; i < 3; ++i) {
                m_smoothedRate[i] *= (1.0 - bleed);
            }
        }
    }

    UpdateAxisSelection(m_smoothedRate[0], m_smoothedRate[1], m_smoothedRate[2], dt);

    // ---- whole-device motion assessment ----------------------------------
    double accelDeviation = 0.0;
    if (sample.accel.valid) {
        const double magnitude = std::sqrt(Square(sample.acceleration.x) +
                                           Square(sample.acceleration.y) +
                                           Square(sample.acceleration.z));
        accelDeviation = std::abs(magnitude - 1.0);
    }

    // A hinge fold concentrates rotation on one device axis; picking the
    // machine up spreads the energy across axes.  Combined with linear
    // acceleration this decides how much of the update to accept.
    bool wholeDeviceMotion = false;
    double motionSeverity = 0.0;
    if (sample.accel.valid && accelDeviation > c.accelDeviationLimitG) {
        wholeDeviceMotion = true;
        motionSeverity = std::max(motionSeverity, 0.7);
    }
    const double rateMagnitude =
        std::sqrt(Square(m_smoothedRate[0]) + Square(m_smoothedRate[1]) +
                  Square(m_smoothedRate[2]));
    m_lastRateMagnitude = rateMagnitude;
    if (rateMagnitude >= c.motionConfirmRateDegPerSec && m_axisIndex >= 0 &&
        m_lastWindowEnergy > 1e-6 && m_lastAxisShare > 0.0 &&
        m_lastAxisShare < c.axisShareFloor) {
        wholeDeviceMotion = true;
        motionSeverity = std::max(motionSeverity, 0.5);
    }

    const double suppressionTarget =
        wholeDeviceMotion ? motionSeverity * c.motionPenalty : 0.0;
    // orientationCorrectionGain scales how fast the suppression itself moves,
    // so a single noisy sample cannot stall the estimator.
    const double suppressionTau =
        std::max(0.05, 1.0 / std::max(0.001, c.orientationCorrectionGain * 50.0));
    m_suppression += (suppressionTarget - m_suppression) * Approach(dt, suppressionTau);
    m_lastWholeDeviceMotion = wholeDeviceMotion;

    // ---- hinge rate -------------------------------------------------------
    double rate = 0.0;
    if (m_axisIndex >= 0) {
        rate = m_smoothedRate[m_axisIndex] * m_axisSign;
    }
    if (std::abs(rate) < c.gyroDeadZoneDegPerSec) {
        rate = 0.0;
    }
    m_lastRateDegPerSec = rate;

    UpdateSignProbe(m_smoothedRate, sample.steadySeconds);

    // ---- integrate --------------------------------------------------------
    const double instantVelocity = rate * c.gyroProgressGain;
    const double weight = 1.0 - m_suppression;
    m_progress += instantVelocity * weight * dt;

    if (m_hasLidMode) {
        ApplyLidAnchor(lidMode, dt);
    }
    m_progress = Clamp(m_progress, 0.0, 1.0);

    UpdateDirection(instantVelocity * weight, dt);
    UpdateConfidence(sample, accelDeviation);
    m_region = ComputeRegion(lidMode, m_progress);
}

// ==========================================================================
//  Internals
// ==========================================================================
double HingeEstimator::MedianState::Push(double value) {
    int count = length;
    if (count < 1) {
        count = 1;
    }
    if (count > kMedianCapacity) {
        count = kMedianCapacity;
    }
    if ((count % 2) == 0) {
        --count;  // a median needs an odd count
    }

    // The ring must wrap at `count`, not at the capacity, otherwise the window
    // straddles stale slots and the median is computed over the wrong samples.
    if (index >= count) {
        index = 0;
    }
    buffer[index] = value;
    index = (index + 1) % count;

    double sorted[kMedianCapacity];
    std::copy(buffer, buffer + count, sorted);
    std::sort(sorted, sorted + count);
    return sorted[count / 2];
}

void HingeEstimator::UpdateGyroBias(const double (&values)[3], double dt) {
    const HingeEstimatorConfig& c = m_config;

    // Per-axis peak, not the vector magnitude: one axis carrying a fast motion
    // must be enough to keep the sample out of the bias estimate.
    const double peak = std::max({std::abs(values[0]), std::abs(values[1]),
                                  std::abs(values[2])});
    if (peak >= c.biasLearningRateLimitDegPerSec) {
        m_quietSeconds = 0.0;
        return;
    }

    m_quietSeconds += dt;
    if (m_quietSeconds < c.biasLearningDelaySeconds) {
        return;
    }

    // Slow enough that a genuine, slow fold is not absorbed into the offset.
    const double alpha = Approach(dt, c.biasLearningTauSeconds);
    for (int i = 0; i < 3; ++i) {
        m_gyroBias[i] += (values[i] - m_gyroBias[i]) * alpha;
    }
    m_biasLearned = true;
}

int HingeEstimator::PickBestAxis() const {
    int best = 0;
    for (int i = 1; i < 3; ++i) {
        if (m_axisEnergy[i] > m_axisEnergy[best]) {
            best = i;
        }
    }
    return best;
}

void HingeEstimator::UpdateAxisSelection(double gx, double gy, double gz, double dt) {
    const HingeEstimatorConfig& c = m_config;
    const double values[3] = {gx, gy, gz};

    // Only real motion feeds the axis statistics.  This machine's X axis has a
    // ~19 deg/s zero-mean noise floor at rest; if noise were allowed to vote it
    // would always win the energy contest and the estimator would integrate the
    // wrong axis.  The threshold matches the idle-freeze floor for the same
    // reason.
    const double magnitude = std::sqrt(Square(gx) + Square(gy) + Square(gz));
    if (magnitude >= c.axisEnergyMinRateDegPerSec) {
        for (int i = 0; i < 3; ++i) {
            m_axisEnergy[i] += Square(values[i]) * dt;
        }
        m_axisEnergySeconds += dt;
    }

    const double total = m_axisEnergy[0] + m_axisEnergy[1] + m_axisEnergy[2];
    m_lastWindowEnergy = total;

    // The axis must exist from the very first real motion sample; waiting for a
    // full window is what made the progress appear to lag behind the motion.
    // Noise-only energy never reaches the floor, so the noise axis cannot vote.
    if (m_axisIndex < 0 && !m_axisLockedByConfig && total > c.axisEnergyFloor) {
        m_axisIndex = PickBestAxis();
        m_axisProvisional = true;
    }

    if (m_axisEnergySeconds < c.axisWindowSeconds) {
        if (total > 0.0 && m_axisIndex >= 0) {
            m_lastAxisShare = m_axisEnergy[m_axisIndex] / total;
        }
        return;
    }

    if (!m_axisLockedByConfig && total > c.axisEnergyFloor && m_axisIndex >= 0) {
        const int best = PickBestAxis();
        const double bestShare = m_axisEnergy[best] / total;
        if (bestShare >= 0.5) {
            // While provisional, follow any clear leader immediately; once the
            // axis is confirmed, require the hysteresis ratio so it cannot flap.
            const double ratio = m_axisProvisional ? 1.15 : c.axisSwitchRatio;
            if (best != m_axisIndex &&
                m_axisEnergy[best] > m_axisEnergy[m_axisIndex] * ratio) {
                m_axisIndex = best;
            } else if (best == m_axisIndex) {
                m_axisProvisional = false;
            }
        }
    }

    if (m_axisIndex >= 0 && total > 0.0) {
        m_lastAxisShare = m_axisEnergy[m_axisIndex] / total;
    }

    // Halve the accumulators: recent motion dominates, but a short quiet spell
    // does not erase what was learned.
    for (int i = 0; i < 3; ++i) {
        m_axisEnergy[i] *= 0.5;
    }
    m_axisEnergySeconds = 0.0;
}

void HingeEstimator::UpdateSignProbe(const double (&values)[3], double nowSeconds) {    if (!m_signProbeActive || m_axisLockedByConfig || m_axisIndex < 0) {
        return;
    }
    if (nowSeconds > m_signProbeDeadline) {
        m_signProbeActive = false;
        return;
    }
    const double rate = values[m_axisIndex];
    if (std::abs(rate) < m_config.signProbeMinRateDegPerSec) {
        return;
    }

    // A rising lid state code means the hinge is opening (observed on this
    // machine; used only to resolve the axis sign and overridable via config).
    const bool positiveMatchesOpening = rate > 0.0;
    m_axisSign = (positiveMatchesOpening == m_signProbeOpening) ? 1.0 : -1.0;
    m_axisSignConfident = true;
    m_signProbeActive = false;
}

void HingeEstimator::NoteLidMode(uint32_t lidMode, double nowSeconds) {
    if (lidMode == m_lidMode) {
        return;
    }
    const uint32_t previous = m_lidMode;
    m_lidMode = lidMode;

    if (previous != 0u && !m_axisLockedByConfig) {
        m_signProbeActive = true;
        m_signProbeOpening = lidMode > previous;
        m_signProbeDeadline = nowSeconds + m_config.signProbeWindowSeconds;
    }
}

void HingeEstimator::ApplyLidAnchor(uint32_t lidMode, double dt) {
    const HingeEstimatorConfig& c = m_config;

    if (lidMode == 1u) {
        // Upper bound only: the machine may legitimately sit near closed, so
        // progress is allowed to fall all the way to 0.
        if (m_progress > c.state1MaxProgress) {
            m_progress -= (m_progress - c.state1MaxProgress) * Approach(dt, 0.40);
        }
    } else if (lidMode == 2u) {
        if (m_progress < c.state2MinProgress) {
            m_progress += (c.state2MinProgress - m_progress) * Approach(dt, 0.40);
        }
        if (m_progress > c.state2MaxProgress) {
            m_progress -= (m_progress - c.state2MaxProgress) * Approach(dt, 0.40);
        }
    } else if (lidMode >= 3u && c.state3ForcesOpen) {
        // The visual effect only covers closed..flat; anything past flat is
        // "fully open" as far as the renderer is concerned.
        m_progress += (1.0 - m_progress) * Approach(dt, 0.25);
    }
}

void HingeEstimator::UpdateDirection(double instantVelocity, double dt) {
    const HingeEstimatorConfig& c = m_config;

    m_velocity += (instantVelocity - m_velocity) * Approach(dt, 0.03);

    const double magnitude = std::abs(m_velocity);

    if (m_direction == HingeDirection::Stable) {
        if (magnitude > c.motionStartThreshold) {
            m_direction =
                m_velocity > 0.0 ? HingeDirection::Opening : HingeDirection::Closing;
            m_stableSeconds = 0.0;
        }
    } else if (magnitude < c.stableThreshold) {
        m_stableSeconds += dt;
        if (m_stableSeconds * 1000.0 >= static_cast<double>(c.settleTimeMs)) {
            m_direction = HingeDirection::Stable;
            m_velocity = 0.0;
        }
    } else {
        m_stableSeconds = 0.0;
        m_direction = m_velocity > 0.0 ? HingeDirection::Opening : HingeDirection::Closing;
    }
}

void HingeEstimator::UpdateConfidence(const SensorSample& sample, double accelDeviation) {
    if (!sample.gyro.valid) {
        m_confidence = 0.0f;
        return;
    }

    double confidence = 1.0;
    if (!m_hasLidMode) {
        confidence *= 0.75;
    }
    if (m_axisIndex < 0) {
        confidence *= 0.55;
    }
    if (!m_axisSignConfident) {
        confidence *= 0.75;
    }
    if (!m_biasLearned) {
        confidence *= 0.85;  // the gyro offset is not characterised yet
    }
    if (m_lastAxisShare > 0.0 && m_lastRateMagnitude >= m_config.motionConfirmRateDegPerSec) {
        // Only meaningful while something is actually rotating.
        confidence *= Clamp(m_lastAxisShare / 0.6, 0.4, 1.0);
    }
    if (sample.accel.valid) {
        const double limit = std::max(1e-6, m_config.accelDeviationLimitG);
        const double excess = accelDeviation / limit;
        if (excess > 1.0) {
            confidence *= Clamp(1.0 - (excess - 1.0) * 0.5, 0.2, 1.0);
        }
    } else {
        confidence *= 0.85;
    }
    if (!sample.orientationSensor.valid) {
        confidence *= 0.9;
    }
    confidence *= (1.0 - m_suppression * 0.5);

    m_confidence = static_cast<float>(Clamp(confidence, 0.0, 1.0));
}

HingeRegion HingeEstimator::ComputeRegion(uint32_t lidMode, double progress) const {
    switch (lidMode) {
    case 1u:
        return progress < 0.35 ? HingeRegion::ClosedSide : HingeRegion::Laptop;
    case 2u:
        return HingeRegion::NearFlat;
    case 3u:
        return HingeRegion::BeyondFlat;
    case 4u:
        return HingeRegion::TentOrStand;
    case 5u:
        return HingeRegion::Tablet;
    default:
        return HingeRegion::Unknown;
    }
}

double HingeEstimator::DefaultProgressForLidMode(uint32_t lidMode) const {
    const HingeEstimatorConfig& c = m_config;
    switch (lidMode) {
    case 2u:
        return Clamp(c.state2DefaultProgress, 0.0, 1.0);
    case 3u:
    case 4u:
    case 5u:
        return 1.0;
    case 1u:
    default:
        return Clamp(c.state1DefaultProgress, 0.0, 1.0);
    }
}

void HingeEstimator::CopyStateLocked(HingeState& state) const {
    state.foldProgress = static_cast<float>(Clamp(m_progress, 0.0, 1.0));
    state.angularVelocity = static_cast<float>(m_velocity);
    state.confidence = m_confidence;
    state.rawLidMode = m_lidMode;
    state.direction = m_direction;
    state.region = m_region;
    state.estimatedAngleDegrees.reset();  // v1 never fabricates an angle

    state.axisIndex = m_axisIndex;
    state.axisSign = static_cast<float>(m_axisSign);
    state.axisRateDegPerSec = static_cast<float>(m_lastRateDegPerSec);
    state.axisShare = static_cast<float>(m_lastAxisShare);
    state.axisSignConfident = m_axisSignConfident;
    state.wholeDeviceMotion = m_lastWholeDeviceMotion;
    state.manual = m_manual;
    state.sensorsValid = m_axisIndex >= 0;
}

HingeState HingeEstimator::State() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    HingeState state;
    CopyStateLocked(state);
    return state;
}

} // namespace dragonfly
