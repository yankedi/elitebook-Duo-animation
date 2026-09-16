// ---------------------------------------------------------------------------
//  FoldEffectMode.h
//
//  The final goal: while the laptop lid is moving, the desktop itself is shown
//  folding, then handed back untouched once the lid settles.
//
//  Pipeline:
//
//      OrientationSensor -> hingeAngle -> foldProgress        (the sensor layer)
//              |
//              +-- progress < 1  -> capture desktop -> fold shader -> overlay
//              +-- progress = 1  -> hide overlay, stop rendering
//
//  That is duo-open's overlay lifecycle (capture -> show -> follow -> dismiss)
//  rebuilt on Win32 + DXGI: the overlay is only on screen while it has
//  something meaningful to draw, so a stable desktop is never covered.
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>

#include "../app/ConsoleUi.h"

namespace dragonfly {

class SensorManager;
class CustomSensorManager;

struct FoldEffectOptions {
    // ---------------------------------------------------------------------
    //  Strategy taken from lid-plane (jh3y/lid-plane, GPL-3.0-or-later).
    //
    //  The effect is keyed to how far the lid has closed BELOW an activation
    //  angle, not to the absolute hinge angle:
    //
    //      delta = activationAngle - hingeAngle
    //
    //      hingeAngle > activationAngle -> delta < 0 -> desktop untouched
    //      hingeAngle = activationAngle -> delta = 0 -> effect starts
    //      hingeAngle < activationAngle -> delta > 0 -> effect builds
    //
    //  110 degrees is that project's default.  Above it a laptop is simply
    //  being used and the desktop must be left alone; the effect belongs to the
    //  act of closing.  The illusion is that the content holds the activation
    //  angle while the physical screen tilts around it.
    // ---------------------------------------------------------------------
    float activationAngleDeg = 110.0f;

    // Largest angle delta fed to the shader.  lid-plane clamps its delta to
    // roughly 1.25 rad (72 degrees).
    float maxDeltaDegrees = 60.0f;

    // Blur radius per 1000 px of display height, at the largest delta.  This is
    // lid-plane's constant; the shader turns it into pixels using the actual
    // display height so the look scales with panel size.
    float blurStrength = 65.0f;

    // How long the environment has to stay healthy after a lid close, a display
    // power transition or a mode change before the effect is allowed back.
    //
    // The reference uses 0.5 s, which is too long here: opening a lid is a
    // ~1 second motion, so half a second of settling eats most of it and the
    // desktop looks untouched while the lid is already up.  What the delay is
    // really for is to avoid sampling a compositor that is still rebuilding --
    // and the panel's own power-on latency already covers most of that.
    double recoverySettleSeconds = 0.12;

    // Fraction of light lost per pixel of blur radius.
    float darkening = 0.015f;

    // Render loop rate.  The effect only runs while the lid moves, so this is
    // the rate during motion, not an idle cost.
    double rateHz = 60.0;

    // Stop after this many seconds (0 = run until stopped).
    double seconds = 0.0;

    // Save the captured desktop and the rendered result as BMP files.  Used to
    // inspect what the shader actually produced: GDI screen captures cannot see
    // D3D-rendered content, so this is the only reliable view.
    bool dumpFrames = false;

    // Ask Windows to keep the panel powered while the effect is running.
    //
    // A blanked panel has to be woken before anything can be shown on it, and
    // that wake is pure latency: it is why the desktop appears "late" when the
    // lid comes back up.  ES_DISPLAY_REQUIRED stops the idle timeout from
    // blanking it in the first place, so there is nothing to wake.
    //
    // This only covers the idle timeout.  If the platform blanks the panel
    // because the lid itself was shut, no user-mode program can hurry that up,
    // and the settle period above is what covers the rest.
    bool keepDisplayAwake = true;
};

// Returns 0 on success, non-zero when the effect could not be started.
int RunFoldEffect(const FoldEffectOptions& options, SensorManager& sensors,
                  CustomSensorManager& customSensors,
                  std::atomic<bool>& stopFlag, ui::Terminal& terminal);

} // namespace dragonfly
