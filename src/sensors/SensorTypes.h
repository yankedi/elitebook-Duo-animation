// ---------------------------------------------------------------------------
//  SensorTypes.h
//
//  Shared value types for the whole diagnostic stack.
//
//  Design note (phase 1): nothing in here estimates a hinge angle.  The types
//  exist so that the raw hardware data can be logged, replayed and later fed
//  into `HingeEstimator` without touching the acquisition layer again.
// ---------------------------------------------------------------------------
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dragonfly {

// --------------------------------------------------------------------------
//  Small math types (double precision: this is an offline-analysis tool, the
//  extra mantissa bits cost nothing at 50 Hz and avoid rounding surprises in
//  the CSV once the estimator starts differencing these values).
// --------------------------------------------------------------------------
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Quat {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;
};

struct Mat3 {
    // Row-major, m[row * 3 + column].
    double m[9] = {1.0, 0.0, 0.0,
                   0.0, 1.0, 0.0,
                   0.0, 0.0, 1.0};
};

// --------------------------------------------------------------------------
//  Freshness of a single channel.  A sensor that exists but stops delivering
//  is distinguishable from one that was never present: `valid` never flips
//  back once data arrived, but `steadySeconds` stops advancing.
// --------------------------------------------------------------------------
struct ChannelStamp {
    bool valid = false;          // at least one reading has been received
    double steadySeconds = 0.0;  // program-relative time of the last reading

    double AgeSeconds(double nowSteady) const {
        return valid ? (nowSteady - steadySeconds) : -1.0;
    }
};

// --------------------------------------------------------------------------
//  One consistent point-in-time snapshot of every channel.
// --------------------------------------------------------------------------
struct SensorSample {
    double steadySeconds = 0.0;  // monotonic seconds since process start
    int64_t wallUnixMs = 0;      // wall clock, for aligning with a camera / notes

    Vec3 acceleration;           // g (WinRT reports g-force, NOT m/s^2)
    Vec3 angularVelocity;        // rad/s, device axes
    Quat orientation;            // unit quaternion (world -> device)
    Mat3 rotationMatrix;         // fused rotation matrix

    double pitchDeg = 0.0;       // Inclinometer
    double rollDeg = 0.0;
    double yawDeg = 0.0;

    double headingDeg = 0.0;     // Compass, magnetic north

    double hingeAngleDeg = 0.0;  // HingeAngleSensor (null on this machine)
    int lidMode = -1;            // Intel Lid Mode raw value, -1 = unavailable
    int simpleOrientation = -1;  // Windows SimpleOrientation enum, -1 = unavailable

    ChannelStamp accel;
    ChannelStamp gyro;
    ChannelStamp orientationSensor;
    ChannelStamp inclinometer;
    ChannelStamp compass;
    ChannelStamp simpleOrientationChannel;
    ChannelStamp hingeAngle;
    ChannelStamp lidModeChannel;
};

// --------------------------------------------------------------------------
//  HingeState / HingeEstimator types now live in src/hinge/HingeTypes.h
//  (Milestone 5A).  They are deliberately NOT declared here so that the sensor
//  layer stays free of any estimation logic.
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
//  Time helpers.  steady_clock on MSVC is backed by QueryPerformanceCounter,
//  and the epoch is frozen on first use, so every module reports the same
//  program-relative timeline.
// --------------------------------------------------------------------------
inline double SteadySeconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

inline int64_t WallUnixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// --------------------------------------------------------------------------
//  Formatting helpers shared by the console UI and the CSV writer.
// --------------------------------------------------------------------------
inline std::string FormatDouble(double value, int decimals) {
    if (!std::isfinite(value)) {
        return {};
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

} // namespace dragonfly
