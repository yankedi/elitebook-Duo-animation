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

    // Largest angle delta fed to the shader.
    //
    // The reference clamps at about 1.25 rad (72 deg), which leaves the whole
    // closed half of a laptop's travel pinned at maximum blur: on a laptop the
    // lid goes all the way to 0 degrees, so 60 or 72 degrees of clamp means
    // every hinge angle below ~50 degrees renders identically.  Once the effect
    // starts early enough to be seen in that band, it reads as a frozen image.
    //
    // 90 degrees is where sin() peaks, so over the range that is actually
    // visible (hinge 110 down to about 25 degrees) the blur keeps responding to
    // the lid, and the remaining plateau lands below 20 degrees of hinge, where
    // the panel faces the keyboard.
    float maxDeltaDegrees = 90.0f;

    // ---------------------------------------------------------------------
    //  Glass presets.
    //
    //  "reference" reproduces the look of lid-plane's own renderer: the picture
    //  is a lit rectangle hanging in a near-black void, blurred by the gap
    //  between it and the pane, with no tinting or darkening of the picture
    //  itself.  "glass" adds this project's own glass cues on top of that (see
    //  the shader header).  "plain" glues the picture to the panel and drops
    //  every cue, which is the model this project started with and is kept only
    //  so the difference can be seen side by side.
    // ---------------------------------------------------------------------
    enum class GlassPreset { Reference, Glass, Plain };
    GlassPreset glassPreset = GlassPreset::Reference;

    // ---- material overrides, applied after the preset ---------------------
    // Negative means "keep the preset".  Judging a material is a loop of
    // "change one number, look again", and a rebuild inside that loop wastes a
    // minute every time.
    float blurOverride = -1.0f;
    float dispersionOverride = -1.0f;
    float sheenOverride = -1.0f;
    float edgeOverride = -1.0f;

    // How far the eye is from the picture, in millimetres.  Zero or less means
    // "use eyeDistanceHeights" below.  This is the knob that decides how much
    // the picture slides across the pane as the lid moves: it is a real viewing
    // distance, because a projection is only stationary for a viewer standing
    // where the shader thinks they are.
    float eyeDistanceMm = 450.0f;

    // The same distance in screen heights, used when eyeDistanceMm is unset:
    // the reference's own (0, 0.65, 1.6).
    float eyeDistanceHeights = 2.7f;
    float eyeHeightHeights = 0.55f;

    // How far the picture hangs behind the pane's plane, as a fraction of the
    // eye distance.  Zero puts it back on the plane, where the picture swells
    // as the lid closes; larger values hold it stiller and show more of the
    // void around it.  --depth=N overrides.
    float screenDepthRatio = 0.45f;

    // The picture's size relative to the glass.  1 fills the pane at the anchor;
    // less hangs it in space with a margin around it.  --picture=N overrides.
    float pictureScale = 0.85f;

    // Whether the pane window is layered.  Layering is what makes the window
    // click-through; --no-layered-overlay turns it off to test whether a layered
    // full-screen window is what makes something underneath flicker.
    bool layeredOverlay = true;

    // Overrides the preset's parallax (0 = glued to the panel, 1 = anchored).
    float parallaxOverride = -1.0f;

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

    // Hold ES_SYSTEM_REQUIRED while the lid is shut, so the machine cannot drop
    // into Modern Standby (S0 low power idle) behind a closed lid.
    //
    // Coming out of Modern Standby costs a second or more of firmware and driver
    // resume before the panel has any chance to light up, which is the other
    // half of "the screen comes back late".  Off by default: while this is held
    // the machine will not sleep at all with the lid shut, which is a real
    // battery and thermals trade -- opting in should be a decision, not a
    // surprise.
    bool keepSystemAwake = false;

    // How old the held snapshot may be before it stops counting as drawable.
    //
    // A fold that starts while the duplication happens to be dead can still be
    // drawn from the last frame captured -- the desktop is static whenever the
    // panel is off, which is exactly when the duplication dies -- and waiting
    // for a fresh capture is what leaves the first half of a fast opening
    // untouched.  The bound is what keeps that from turning into "the effect
    // shows a desktop from an hour ago" after a long sleep.
    double contentMaxAgeSeconds = 300.0;

    // Render the effect at a fixed angle and stay there, ignoring the lid.
    //
    // The material is a matter of taste and it can only be judged by looking at
    // it, so this turns "close the lid, open it, look, quit, rebuild" into
    // "look".  Zero disables it.  The phase in the status line reads PREV so a
    // preview is never mistaken for a real fold.
    double previewDeltaDegrees = 0.0;
};

// Returns 0 on success, non-zero when the effect could not be started.
int RunFoldEffect(const FoldEffectOptions& options, SensorManager& sensors,
                  CustomSensorManager& customSensors,
                  std::atomic<bool>& stopFlag, ui::Terminal& terminal);

} // namespace dragonfly
