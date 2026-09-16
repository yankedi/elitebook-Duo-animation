// ---------------------------------------------------------------------------
//  OrientationTracker.cpp
// ---------------------------------------------------------------------------
#include "OrientationTracker.h"

#include <algorithm>
#include <cmath>

namespace dragonfly {

namespace {

constexpr double kRadToDeg = 57.29577951308232;

// cos(85 degrees): |beta| beyond this counts as "at the singularity".
constexpr double kGimbalCosThreshold = 0.0872;

double Clamp(double value, double low, double high) {
    return std::min(std::max(value, low), high);
}

double Approach(double dt, double tau) {
    return (tau > 0.0) ? (1.0 - std::exp(-dt / tau)) : 1.0;
}

Mat3 TransposeMatrix(const Mat3& m) {
    Mat3 out;
    out.m[0] = m.m[0]; out.m[1] = m.m[3]; out.m[2] = m.m[6];
    out.m[3] = m.m[1]; out.m[4] = m.m[4]; out.m[5] = m.m[7];
    out.m[6] = m.m[2]; out.m[7] = m.m[5]; out.m[8] = m.m[8];
    return out;
}

// Smoothing matrix elements drifts them off a pure rotation; re-orthonormalise
// with Gram-Schmidt over the rows (row-vector convention).
void Orthonormalize(Mat3& m) {
    double* r0 = &m.m[0];
    double* r1 = &m.m[3];
    double* r2 = &m.m[6];

    auto length = [](const double* v) {
        return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    };
    auto scale = [&](double* v, double factor) {
        v[0] *= factor;
        v[1] *= factor;
        v[2] *= factor;
    };
    auto dot = [](const double* a, const double* b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    auto subtractScaled = [](double* dst, const double* src, double factor) {
        dst[0] -= src[0] * factor;
        dst[1] -= src[1] * factor;
        dst[2] -= src[2] * factor;
    };

    const double len0 = length(r0);
    if (len0 < 1e-9) {
        m = Mat3{};
        return;
    }
    scale(r0, 1.0 / len0);

    subtractScaled(r1, r0, dot(r1, r0));
    const double len1 = length(r1);
    if (len1 < 1e-9) {
        m = Mat3{};
        return;
    }
    scale(r1, 1.0 / len1);

    r2[0] = r0[1] * r1[2] - r0[2] * r1[1];
    r2[1] = r0[2] * r1[0] - r0[0] * r1[2];
    r2[2] = r0[0] * r1[1] - r0[1] * r1[0];
}

// Shortest angular difference, so the smoothing never takes the long way round
// when an angle crosses +/-180.
double ShortestDelta(double from, double to) {
    double delta = to - from;
    while (delta > 180.0) {
        delta -= 360.0;
    }
    while (delta < -180.0) {
        delta += 360.0;
    }
    return delta;
}

} // namespace

// ==========================================================================
//  W3C angle extraction
// ==========================================================================
OrientationAngles ExtractDeviceOrientation(const Mat3& deviceToWorld) {
    // W3C defines R = Rz(alpha) * Rx(beta) * Ry(gamma), which expands to
    //   R[2][1] =  sin(beta)
    //   R[2][0] = -cos(beta) sin(gamma),  R[2][2] = cos(beta) cos(gamma)
    //   R[0][1] = -sin(alpha) cos(beta),  R[1][1] = cos(alpha) cos(beta)
    // (row-major indexing).
    const double* m = deviceToWorld.m;

    OrientationAngles angles;
    const double sinBeta = Clamp(m[3 * 2 + 1], -1.0, 1.0);
    angles.betaDeg = std::asin(sinBeta) * kRadToDeg;
    angles.gammaDeg = std::atan2(-m[3 * 2 + 0], m[3 * 2 + 2]) * kRadToDeg;
    angles.alphaDeg = std::atan2(-m[3 * 0 + 1], m[3 * 1 + 1]) * kRadToDeg;
    if (angles.alphaDeg < 0.0) {
        angles.alphaDeg += 360.0;
    }

    // |cos(beta)| collapses to zero at the singularity, where the gamma and
    // alpha terms above are both built from values that have gone to zero.
    const double cosBeta = std::sqrt(std::max(0.0, 1.0 - sinBeta * sinBeta));
    angles.gimbalLock = cosBeta < kGimbalCosThreshold;
    angles.valid = true;
    return angles;
}

// ==========================================================================
//  OrientationTracker
// ==========================================================================
void OrientationTracker::Configure(double smoothingTauSeconds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tau = Clamp(smoothingTauSeconds, 0.001, 2.0);
}

void OrientationTracker::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_valid = false;
    m_rotation = Mat3{};
    m_smoothedRotation = Mat3{};
    m_rawAngles = OrientationAngles{};
    m_smoothedAngles = OrientationAngles{};
    m_hasReference = false;
    m_worldToReference = Mat3{};
}

void OrientationTracker::SetTranspose(bool transpose) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_transpose != transpose) {
        m_transpose = transpose;
        m_valid = false;  // re-seed on the next sample
    }
}

bool OrientationTracker::Transposed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_transpose;
}

void OrientationTracker::Update(const SensorSample& sample, double dtSeconds) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!sample.orientationSensor.valid) {
        return;
    }

    double dt = dtSeconds;
    if (!(dt > 0.0) || dt > 0.25) {
        dt = 1.0 / 60.0;
    }

    Mat3 rotation = sample.rotationMatrix;
    if (m_transpose) {
        rotation = TransposeMatrix(rotation);
    }

    m_rotation = rotation;

    OrientationAngles extracted = ExtractDeviceOrientation(rotation);

    // ---- keep beta continuous across the +/-90 fold ------------------------
    // (a, b, g) and (a+180, 180-b, g+180) describe the same rotation (this
    // follows from Rz(180) * Rx(180) = Ry(180)).  When beta folds at the
    // singularity, switch to the equivalent branch so the angle keeps growing
    // instead of mirroring back -- that mirroring is exactly what reads as a
    // sudden 180 degree flip.
    if (m_valid && m_smoothedAngles.valid) {
        const double previousBeta = m_smoothedAngles.betaDeg;

        const double directBeta = extracted.betaDeg;
        double foldedBeta = (directBeta >= 0.0 ? 180.0 : -180.0) - directBeta;
        while (foldedBeta > 180.0) {
            foldedBeta -= 360.0;
        }
        while (foldedBeta < -180.0) {
            foldedBeta += 360.0;
        }

        const double directDistance = std::abs(ShortestDelta(previousBeta, directBeta));
        const double foldedDistance = std::abs(ShortestDelta(previousBeta, foldedBeta));

        // 10 degrees of hysteresis so the branch cannot oscillate.
        if (foldedDistance + 10.0 < directDistance) {
            extracted.betaDeg = foldedBeta;
            extracted.gammaDeg += 180.0;
            extracted.alphaDeg += 180.0;
            extracted.foldedBeta = true;
        }
    }

    // ---- freeze the unobservable pair at the singularity -------------------
    // At the singularity alpha and gamma are not separately observable (only
    // their sum is), so chasing them just produces noise-driven flips.  Beta
    // stays valid throughout.
    if (extracted.gimbalLock && m_valid) {
        extracted.gammaDeg = m_smoothedAngles.gammaDeg;
        extracted.alphaDeg = m_smoothedAngles.alphaDeg;
    }

    m_rawAngles = extracted;

    if (!m_valid) {
        m_smoothedRotation = rotation;
        m_smoothedAngles = extracted;
        m_valid = true;
        return;
    }

    const double blend = Approach(dt, m_tau);

    for (int i = 0; i < 9; ++i) {
        m_smoothedRotation.m[i] += (rotation.m[i] - m_smoothedRotation.m[i]) * blend;
    }
    Orthonormalize(m_smoothedRotation);

    m_smoothedAngles.alphaDeg +=
        ShortestDelta(m_smoothedAngles.alphaDeg, extracted.alphaDeg) * blend;
    m_smoothedAngles.betaDeg +=
        ShortestDelta(m_smoothedAngles.betaDeg, extracted.betaDeg) * blend;
    m_smoothedAngles.gammaDeg +=
        ShortestDelta(m_smoothedAngles.gammaDeg, extracted.gammaDeg) * blend;
    m_smoothedAngles.gimbalLock = extracted.gimbalLock;
    m_smoothedAngles.foldedBeta = extracted.foldedBeta;
    m_smoothedAngles.valid = true;

    // Display convention: alpha wraps into 0..360 and gamma must stay bounded.
    // Without the gamma wrap the shortest-delta smoothing walks it around the
    // circle indefinitely (it reached -3420 degrees in testing), which then
    // drives any gamma-based visual completely wrong.
    while (m_smoothedAngles.alphaDeg < 0.0) {
        m_smoothedAngles.alphaDeg += 360.0;
    }
    while (m_smoothedAngles.alphaDeg >= 360.0) {
        m_smoothedAngles.alphaDeg -= 360.0;
    }
    while (m_smoothedAngles.gammaDeg > 180.0) {
        m_smoothedAngles.gammaDeg -= 360.0;
    }
    while (m_smoothedAngles.gammaDeg < -180.0) {
        m_smoothedAngles.gammaDeg += 360.0;
    }
    while (m_smoothedAngles.betaDeg > 180.0) {
        m_smoothedAngles.betaDeg -= 360.0;
    }
    while (m_smoothedAngles.betaDeg < -180.0) {
        m_smoothedAngles.betaDeg += 360.0;
    }
}

OrientationAngles OrientationTracker::Angles() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_smoothedAngles;
}

OrientationAngles OrientationTracker::RawAngles() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rawAngles;
}

Mat3 OrientationTracker::Rotation() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_smoothedRotation;
}

void OrientationTracker::Normal(double& x, double& y, double& z) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Windows reports the matrix in the column-vector convention
    // (v_world = R * v_device, verified against the quaternion on this machine),
    // so the device Z axis is the THIRD COLUMN.  Taking the third row instead
    // yields the transpose and inverts the apparent tilt direction.
    x = m_smoothedRotation.m[0 * 3 + 2];
    y = m_smoothedRotation.m[1 * 3 + 2];
    z = m_smoothedRotation.m[2 * 3 + 2];
}

void OrientationTracker::CaptureReference() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_valid) {
        return;
    }

    // World -> reference-device-frame.  Expressed this way, the reference frame
    // IS the scene frame, so the screen starts upright facing the camera and a
    // fixed base can live in the same space.
    const Mat3& r = m_smoothedRotation;
    Mat3 worldToReference;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            worldToReference.m[row * 3 + col] = r.m[col * 3 + row];
        }
    }
    m_worldToReference = worldToReference;
    m_hasReference = true;
}

bool OrientationTracker::HasReference() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_hasReference;
}

void OrientationTracker::RelativeNormal(double& x, double& y, double& z,
                                        int variant) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    double normal[3];
    if (variant == 1) {
        // Third row: the transposed convention.
        normal[0] = m_smoothedRotation.m[2 * 3 + 0];
        normal[1] = m_smoothedRotation.m[2 * 3 + 1];
        normal[2] = m_smoothedRotation.m[2 * 3 + 2];
    } else {
        // Third column: v_world = R * v_device, verified against the quaternion.
        normal[0] = m_smoothedRotation.m[0 * 3 + 2];
        normal[1] = m_smoothedRotation.m[1 * 3 + 2];
        normal[2] = m_smoothedRotation.m[2 * 3 + 2];
    }

    if (!m_hasReference) {
        double out[3] = {normal[0], normal[1], normal[2]};
        if (variant == 2) {
            out[1] = -out[1];
        } else if (variant == 3) {
            out[2] = -out[2];
        }
        x = out[0];
        y = out[1];
        z = out[2];
        return;
    }

    // (R0^T * n)[i] = sum_j (R0^T)[i][j] * n[j].  m_worldToReference already
    // *is* R0^T, so it is indexed directly -- indexing it transposed again
    // would rotate by R0 instead, which leaves the very first frame tilted.
    double rotated[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            rotated[i] += m_worldToReference.m[i * 3 + j] * normal[j];
        }
    }

    if (variant == 2) {
        rotated[1] = -rotated[1];
    } else if (variant == 3) {
        rotated[2] = -rotated[2];
    }

    x = rotated[0];
    y = rotated[1];
    z = rotated[2];
}

Mat3 OrientationTracker::RelativeRotation() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hasReference) {
        return m_smoothedRotation;
    }

    // R_rel = R0^T * R  (column-vector convention), where m_worldToReference
    // already holds R0^T.
    Mat3 out;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += m_worldToReference.m[k * 3 + i] * m_smoothedRotation.m[k * 3 + j];
            }
            out.m[i * 3 + j] = sum;
        }
    }
    return out;
}

bool OrientationTracker::Valid() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_valid;
}

} // namespace dragonfly
