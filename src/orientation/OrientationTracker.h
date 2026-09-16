// ---------------------------------------------------------------------------
//  OrientationTracker.h
//
//  Turns the fused Windows OrientationSensor attitude into the same
//  alpha/beta/gamma triple a browser exposes through DeviceOrientationEvent,
//  and smooths it the same way the analysed site does.
//
//  --- What the analysed site actually does ---------------------------------
//  Public front-end bundle (no source map, no repository link in the markup):
//    https://testyourdevices.com/gyroscope-test/
//    /_astro/GyroscopeTest.astro_astro_type_script_index_0_lang.APJ9bzbN.js
//
//  Relevant excerpt, verbatim:
//      const b = t => {
//        const o = t.alpha, i = t.beta, l = t.gamma;
//        F.textContent = p(o); M.textContent = p(i); O.textContent = p(l);
//        const a = i ?? 0, L = l ?? 0;
//        P.style.transform = `rotateX(${(-a).toFixed(2)}deg) rotateY(${L.toFixed(2)}deg)`;
//        const N = G => Math.max(-90, Math.min(90, G)), $ = 44,
//              A = N(L)/90*$ , B = N(a)/90*$;
//        k.style.transform = `translate(${A.toFixed(1)}px, ${B.toFixed(1)}px)`;
//      };
//
//  Findings that drive this module:
//    * the site consumes the browser's ALREADY FUSED attitude angles
//      (DeviceOrientationEvent.alpha/beta/gamma) directly,
//    * it performs NO gyro integration, NO Kalman/complementary filter and no
//      sensor fusion of its own (DeviceMotionEvent is only printed as numbers),
//    * the only smoothing is a 75 ms CSS transition on the transform property
//      (`.duration-75{transition-duration:75ms}` in the stylesheet),
//    * the visual uses beta and gamma only -- alpha (yaw) never reaches the
//      model, which is a large part of why it looks so steady.
//
//  Windows exposes the very same fused attitude through
//  OrientationSensor.Quaternion() / RotationMatrix().  This class converts it
//  to the W3C angles and applies an equivalent 75 ms low-pass.
// ---------------------------------------------------------------------------
#pragma once

#include "../sensors/SensorTypes.h"

#include <mutex>

namespace dragonfly {

// W3C DeviceOrientation angles (intrinsic Z-X'-Y'' Tait-Bryan).
struct OrientationAngles {
    double alphaDeg = 0.0;  // rotation about Z, compass twist   0..360
    double betaDeg = 0.0;   // rotation about X, front/back tilt -180..180
    double gammaDeg = 0.0;  // rotation about Y, left/right tilt  -90..90
    bool valid = false;

    // ---- diagnostics ------------------------------------------------------
    // True while the extraction sits near the ZXY gimbal lock (|cos beta| ~ 0).
    // There alpha and gamma are not separately observable: only their sum is.
    // A laptop screen held upright parks beta right on that singularity, which
    // is exactly what makes a naive extraction appear to flip 180 degrees.
    bool gimbalLock = false;
    // True when the equivalent representation (a+180, 180-b, g+180) was used
    // to keep beta continuous instead of letting it fold at +/-90.
    bool foldedBeta = false;
};

// Extracts alpha/beta/gamma from a matrix that maps device coordinates to
// world coordinates, using the W3C definition R = Rz(a) * Rx(b) * Ry(g).
OrientationAngles ExtractDeviceOrientation(const Mat3& deviceToWorld);

class OrientationTracker {
public:
    void Configure(double smoothingTauSeconds);
    void Reset();

    // Feeds the fused attitude of one SensorSample (only the orientation
    // channel is used; the gyro is not integrated here).
    void Update(const SensorSample& sample, double dtSeconds);

    // Smoothed values, ready to drive a model exactly like the website does.
    OrientationAngles Angles() const;
    OrientationAngles RawAngles() const;

    // Smoothed attitude matrix (device -> world), re-orthonormalised.  This is
    // the representation to prefer: it has no gimbal lock.
    Mat3 Rotation() const;

    // Screen normal in world coordinates (= third row of the rotation matrix,
    // i.e. where the device's Z axis points).  Continuous everywhere.
    void Normal(double& x, double& y, double& z) const;

    // Angle between the device's Z axis (the screen normal) and the world's up
    // direction, in degrees:
    //      0 = screen lying flat, facing up   (lid closed)
    //     90 = screen upright
    //    180 = screen folded all the way back
    //
    // Only the Z component of the normal is involved, so this quantity is
    // INVARIANT under yaw: turning the whole machine on the desk rotates the
    // normal inside the horizontal plane, which leaves its Z component -- and
    // therefore this angle -- untouched.  That is what stops a whole-device
    // turn from leaking into the apparent lid angle.
    //
    // It is also absolute, not relative to a captured pose, so the model starts
    // out matching the real lid position instead of holding an arbitrary offset.
    double TiltDegrees() const;

    // ---- reference capture -------------------------------------------------
    // Records the current attitude as the reference.  Everything after that is
    // expressed *relative* to it, which is what lets a fixed base and a moving
    // screen live in the same scene: the screen starts upright facing the
    // camera no matter how the machine happens to be oriented in the room.
    void CaptureReference();
    bool HasReference() const;

    // Device Z axis relative to the captured reference, expressed in the
    // reference frame.  At the moment of capture this is exactly (0, 0, 1).
    //
    // `variant` selects the extraction convention so the correct one can be
    // pinned down against the browser on real hardware:
    //   0  third column of the rotation matrix  (v_world = R * v_device)
    //   1  third row                            (transposed convention)
    //   2  as 0, with the relative Y negated
    //   3  as 0, with the relative Z negated
    void RelativeNormal(double& x, double& y, double& z, int variant = 0) const;

    // Full attitude relative to the captured reference (device -> reference
    // frame).  Identity at the moment of capture.
    Mat3 RelativeRotation() const;

    bool Valid() const;

    // The convention of the Windows rotation matrix is verified against the
    // browser values at runtime; this flips it if the machine reports the
    // inverse rotation.
    void SetTranspose(bool transpose);
    bool Transposed() const;

private:
    mutable std::mutex m_mutex;
    double m_tau = 0.075;
    bool m_valid = false;
    bool m_transpose = false;

    Mat3 m_rotation;          // raw, transposed if requested
    Mat3 m_smoothedRotation;  // smoothed + orthonormalised
    OrientationAngles m_rawAngles;
    OrientationAngles m_smoothedAngles;

    // Reference attitude: the world -> reference-device-frame rotation.  At
    // capture time this maps the then-current screen normal onto +Z.
    bool m_hasReference = false;
    Mat3 m_worldToReference;
};

} // namespace dragonfly
