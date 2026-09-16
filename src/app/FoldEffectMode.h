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
    // Tuning, taken from Duo-animation's FoldParameters (see FoldRenderer.h).
    float eyeDistanceMm = 450.0f;
    float blurSpread = 0.12f;
    float darkening = 0.015f;
    float maxTiltDegrees = 62.0f;

    // Render loop rate.  The effect only runs while the lid moves, so this is
    // the rate during motion, not an idle cost.
    double rateHz = 60.0;

    // Stop after this many seconds (0 = run until stopped).
    double seconds = 0.0;

    // Save the captured desktop and the rendered result as BMP files.  Used to
    // inspect what the shader actually produced: GDI screen captures cannot see
    // D3D-rendered content, so this is the only reliable view.
    bool dumpFrames = false;
};

// Returns 0 on success, non-zero when the effect could not be started.
int RunFoldEffect(const FoldEffectOptions& options, SensorManager& sensors,
                  CustomSensorManager& customSensors,
                  std::atomic<bool>& stopFlag, ui::Terminal& terminal);

} // namespace dragonfly
