// ---------------------------------------------------------------------------
//  FoldEffectMode.cpp
// ---------------------------------------------------------------------------
#include "FoldEffectMode.h"

#include "DisplaySafetyGate.h"

#include "../capture/DesktopCapture.h"
#include "../graphics/D3DDevice.h"
#include "../graphics/FoldRenderer.h"
#include "../graphics/FrameDump.h"
#include "../graphics/OverlayWindow.h"
#include "../orientation/OrientationTracker.h"
#include "../sensors/CustomSensorManager.h"
#include "../sensors/SensorManager.h"

#include <windows.h>

#include <wrl/client.h>

#include <conio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace dragonfly {

namespace {

// Creates the texture the shader samples.  The capture's own frame has to be
// released every cycle, so its contents are copied here first.
bool CreateContentTexture(ID3D11Device* device, uint32_t width, uint32_t height,
                          Microsoft::WRL::ComPtr<ID3D11Texture2D>& out) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // matches the duplication output
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(device->CreateTexture2D(&description, nullptr, &out));
}

int PollConsoleKey() {
    if (!_kbhit()) {
        return 0;
    }
    const int key = _getch();
    // Arrow and function keys arrive as a two byte 0x00/0xE0 prefix.
    if (key == 0x00 || key == 0xE0) {
        _getch();
        return 0;
    }
    return key;
}

// Hysteresis for "the lid is shut".  The hinge angle is derived from the panel
// normal, so it is least reliable exactly when the lid is closed; a single
// threshold would flap the safety gate open and shut.
constexpr double kLidClosedDegrees = 8.0;
constexpr double kLidOpenDegrees = 25.0;

std::string FormatLine(const char* phase, double hingeAngle, double angleDelta,
                       double activationAngle, const char* gateText, int lidMode,
                       bool overlayVisible, bool hasContent, uint64_t frames,
                       uint64_t captures) {
    char buffer[300];
    std::snprintf(buffer, sizeof(buffer),
                  "%s  hinge %6.1f   delta %6.1f (act %.0f)   %-28s lid %s   "
                  "overlay %s   content %-3s   frames %llu   grabs %llu",
                  phase, hingeAngle, angleDelta, activationAngle, gateText,
                  lidMode < 0 ? "--" : std::to_string(lidMode).c_str(),
                  overlayVisible ? "ON " : "off", hasContent ? "yes" : "NO",
                  static_cast<unsigned long long>(frames),
                  static_cast<unsigned long long>(captures));
    return buffer;
}

} // namespace

int RunFoldEffect(const FoldEffectOptions& options, SensorManager& sensors,
                  CustomSensorManager& customSensors,
                  std::atomic<bool>& stopFlag, ui::Terminal& terminal) {
    // ---- overlay window ---------------------------------------------------
    OverlayWindow overlay;
    if (!overlay.Create(L"Dragonfly Fold Effect")) {
        terminal.Write("ERROR: could not create the overlay window.\n");
        return 1;
    }
    uint32_t width = overlay.Width();
    uint32_t height = overlay.Height();

    // ---- D3D device -------------------------------------------------------
    D3DDevice device;
    if (!device.Create(overlay.Handle(), width, height)) {
        terminal.Write("ERROR: could not create the D3D11 device for the overlay.\n");
        overlay.Destroy();
        return 1;
    }

    // ---- desktop capture --------------------------------------------------
    DesktopCapture capture;
    if (!capture.Start(device.Device())) {
        terminal.Write("ERROR: desktop capture failed: " + capture.Error() +
                       "\n       (a screen recorder or remote session may already hold it)\n");
        overlay.Destroy();
        return 1;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> content;
    if (!CreateContentTexture(device.Device(), capture.Width(), capture.Height(),
                              content)) {
        terminal.Write("ERROR: could not create the content texture.\n");
        capture.Stop();
        overlay.Destroy();
        return 1;
    }

    // ---- effect shader ----------------------------------------------------
    // Strategy and constants from lid-plane (jh3y/lid-plane, GPL-3.0-or-later):
    //   * the effect is driven by how far the lid has closed below an activation
    //     angle (110 degrees), not by the absolute hinge angle;
    //   * the blur radius is normalised per 1000 px of display height so the
    //     look does not change with resolution;
    //   * the projection keeps the content near identity so the illusion is
    //     "the picture holds its angle while the panel tilts".
    FoldEffectParameters parameters;
    parameters.maxDeltaDegrees = options.maxDeltaDegrees;
    parameters.blurStrength = options.blurStrength;
    parameters.darkening = options.darkening;
    // Keeps the ray-plane projection near identity; see FoldRenderer.h.
    parameters.eyeDistancePx = static_cast<float>(width) * 6.4f;

    const UINT dpi = GetDpiForWindow(overlay.Handle());

    FoldRenderer renderer;
    if (!renderer.Create(device.Device(), width, height)) {
        terminal.Write(std::string("ERROR: fold shader failed: ") +
                       renderer.LastError() + "\n");
        capture.Stop();
        overlay.Destroy();
        return 1;
    }

    // ---- safety gate ------------------------------------------------------
    // Decides whether the effect may be on screen at all; see DisplaySafetyGate.h.
    // Declared here because the banner below reports its recovery delay.
    DisplaySafetyGate safety;
    safety.SetRecoveryDelay(options.recoverySettleSeconds);

    // Keep the panel powered while the effect is running.  Without this the
    // idle timeout blanks it, and the first thing the user sees after opening
    // the lid is the blank panel waking up -- the effect cannot be earlier than
    // the panel is.  Cleared on every exit path at the end of this function.
    if (options.keepDisplayAwake) {
        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    }

    terminal.Write(
        "\nFold effect armed.\n"
        "The desktop is left completely untouched at or above the activation\n"
        "angle.  Close the lid below it and the picture holds the activation\n"
        "angle -- keystoned and progressively blurred -- while the panel tilts.\n"
        "Open back above it and the desktop returns untouched.\n"
        "\n"
        "STOP: press [ESC] or [F10] -- these are read globally, so they work even\n"
        "      though the overlay holds the screen.  [Q] in this console also works.\n\n");
    char setup[600];
    const int initialLidSwitch = overlay.LidSwitchState();
    std::snprintf(setup, sizeof(setup),
                  "  display %ux%u, %u dpi -> eye distance %.0f px\n"
                  "  activation %.0f deg, blur %.0f/1000px, darken %.3f, max delta %.0f deg\n"
                  "  capture %ux%u, DXGI_FORMAT %d, overlay %ux%u\n"
                  "  monitor power %s (notify %s), lid switch %s (notify %s), settle %.2f s\n"
                  "  display kept awake: %s\n\n",
                  width, height, dpi, parameters.eyeDistancePx,
                  options.activationAngleDeg, parameters.blurStrength,
                  parameters.darkening, parameters.maxDeltaDegrees,
                  capture.Width(), capture.Height(), static_cast<int>(capture.Format()),
                  width, height, overlay.DisplayOn() ? "on" : "off",
                  overlay.DisplayNotifyActive() ? "yes" : "NO",
                  initialLidSwitch < 0 ? "unknown"
                                       : (initialLidSwitch != 0 ? "open" : "closed"),
                  overlay.LidNotifyActive() ? "yes" : "NO", safety.RecoveryDelay(),
                  options.keepDisplayAwake ? "yes" : "no");
    terminal.Write(setup);

    // ---- prime the first frame -------------------------------------------
    // The first acquire can hand back a surface the compositor has not filled
    // in yet, so keep asking until something arrives.  duo-open guards against
    // the very same thing with its "is the screenshot mostly black" retry.
    bool hasContent = false;
    for (int attempt = 0; attempt < 20 && !hasContent; ++attempt) {
        if (capture.AcquireFrame(100)) {
            capture.CopyFrameTo(device.Context(), content.Get());
            capture.ReleaseFrame();
            hasContent = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (hasContent) {
        // Let the compositor settle, then refresh once more so the primed frame
        // is a real desktop rather than a half-drawn one.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        if (capture.AcquireFrame(300)) {
            capture.CopyFrameTo(device.Context(), content.Get());
            capture.ReleaseFrame();
        }
        if (options.dumpFrames) {
            if (DumpTextureToBmp(device.Device(), device.Context(), content.Get(),
                                 "fold-dump-content.bmp")) {
                terminal.Write("dumped fold-dump-content.bmp\n");
            } else {
                terminal.Write("WARNING: could not dump the captured content.\n");
            }
        }
        terminal.Write("Desktop frame captured; the effect is live.\n\n");
    } else {
        terminal.Write(
            "WARNING: no desktop frame could be captured yet.\n"
            "         The overlay will stay blank until the screen changes.\n"
            "         If this persists, another capture tool may hold the desktop.\n\n");
    }

    // ---- effect state -----------------------------------------------------
    OrientationTracker tracker;
    tracker.Configure(0.075);

    bool pastFlat = false;
    // Latched, because the hinge estimate is driven by acos() of the panel
    // normal: near the fully closed pose that value sits on the singularity and
    // is far too noisy to be compared against a single threshold every frame.
    bool lidClosedLatch = false;
    double lastSampleSeconds = 0.0;
    double lastReportSeconds = -1.0;
    double lastRecoverySeconds = -1.0;
    double lastRefreshSeconds = 0.0;
    std::size_t lastLineLength = 0;
    bool lastEffectWanted = false;
    uint64_t renderedFrames = 0;
    uint64_t captureCount = 0;
    uint64_t loopCount = 0;
    uint64_t modeChanges = 0;

    // Environment bookkeeping, for the transition log and the gate.
    DisplaySafetyGate::State lastGateState = DisplaySafetyGate::State::Recovering;
    int lastLidSwitch = overlay.LidSwitchState();
    bool lastDisplayOn = overlay.DisplayOn();

    // Grabs the desktop into the content texture.
    //
    // A timeout is NOT a failure here.  AcquireNextFrame only hands back a frame
    // when the composited desktop changed, and it times out when the image is
    // identical to the one it delivered last -- and that delivered image is
    // exactly what the content texture holds.  So "no new frame" means the frame
    // in hand is still the current desktop, and it can be used as is.
    //
    // The duplication being dead is the one real failure: the texture then holds
    // something that can no longer be trusted, and the caller must not draw it.
    auto grabDesktop = [&]() -> bool {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (capture.AcquireFrame(100)) {
                capture.CopyFrameTo(device.Context(), content.Get());
                capture.ReleaseFrame();
                ++captureCount;
                return true;
            }
            if (!capture.Healthy()) {
                hasContent = false;
                return false;
            }
        }
        return hasContent;  // nothing changed: the frame in hand is still current
    };

    // A small event log, so a run can be read back afterwards: the whole point
    // of the safety gate is timing, and the console status line is overwritten
    // every 0.25 s.
    auto note = [&](const char* what, double hingeDeg) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "\n  [env] %-22s hinge %6.1f\n", what,
                      hingeDeg);
        terminal.Write(buffer);
    };

    auto nextTick = std::chrono::steady_clock::now();

    while (!stopFlag.load()) {
        // ---- stop keys ----------------------------------------------------
        // The overlay is topmost and click-through, so the console never holds
        // focus and _kbhit() sees nothing.  GetAsyncKeyState reads the physical
        // key state instead, so ESC works no matter what has focus.
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
            break;
        }
        // F10 as a second way out, in case something swallows ESC.
        if ((GetAsyncKeyState(VK_F10) & 0x8000) != 0) {
            break;
        }

        const int consoleKey = PollConsoleKey();
        if (consoleKey == 27 || consoleKey == 'q' || consoleKey == 'Q') {
            break;
        }

        // ---- window messages ---------------------------------------------
        if (!overlay.PumpMessages()) {
            break;
        }

        // ---- sensors ------------------------------------------------------
        const SensorSample sample = sensors.Snapshot();
        const int lidMode = customSensors.LidMode();

        double dt = 1.0 / 60.0;
        if (lastSampleSeconds > 0.0 && sample.steadySeconds > lastSampleSeconds) {
            dt = sample.steadySeconds - lastSampleSeconds;
        }
        lastSampleSeconds = sample.steadySeconds;

        tracker.Update(sample, dt);

        if (lidMode >= 3) {
            pastFlat = true;
        } else if (lidMode == 1) {
            pastFlat = false;
        }

        const double tiltDegrees = tracker.TiltDegrees();
        const double hingeAngle =
            pastFlat ? (180.0 + tiltDegrees) : (180.0 - tiltDegrees);

        // ---- the effect belongs to the act of closing ----------------------
        // Keyed to how far BELOW the activation angle the lid has come, not to
        // the absolute hinge angle.  At or above the activation angle a laptop
        // is simply being used and the desktop must stay untouched; the effect
        // is reserved for the act of closing, where it produces the illusion
        // that the picture holds its angle while the panel tilts around it.
        const double angleDelta =
            static_cast<double>(options.activationAngleDeg) - hingeAngle;

        // ---- environment safety gate ----------------------------------------
        // Strategy from lid-plane (jh3y/lid-plane, GPL-3.0-or-later): while the
        // lid is shut, the display is not usable or the sensor has stopped
        // delivering, the effect is paused AND its overlay removed.
        //
        // Without this the loop below happily keeps rendering: a shut lid puts
        // delta at its maximum, so the overlay stays up at full blur, frozen,
        // for as long as the lid is closed -- and that stale frame is the first
        // thing the panel shows when it comes back.
        const double sensorAge =
            sample.orientationSensor.AgeSeconds(sample.steadySeconds);
        const bool sensorFresh = sensorAge >= 0.0 && sensorAge <= 1.0;

        // Entering "closed" needs an unambiguous low angle, leaving it needs an
        // unambiguous high one, so the noise near the closed singularity cannot
        // flap the gate.  The ACPI lid switch, when the machine has one, is the
        // most direct signal of all and is used ahead of the angle.
        const int lidSwitch = overlay.LidSwitchState();
        if (lidSwitch == 0 ||
            (sensorFresh && (lidMode == 0 || hingeAngle <= kLidClosedDegrees))) {
            lidClosedLatch = true;
        } else if (lidSwitch == 1 || lidMode == 1 ||
                   (sensorFresh && hingeAngle >= kLidOpenDegrees)) {
            lidClosedLatch = false;
        }

        // ---- environment transition log --------------------------------------
        // The gate is all about timing, and the 0.25 s status line is
        // overwritten in place, so the transitions get their own lines: this is
        // the record that says whether a late effect came from the panel waking
        // up or from something this program did.
        const bool displayOn = overlay.DisplayOn();
        if (displayOn != lastDisplayOn) {
            lastDisplayOn = displayOn;
            note(displayOn ? "monitor power on" : "monitor power off", hingeAngle);
        }
        if (lidSwitch != lastLidSwitch) {
            lastLidSwitch = lidSwitch;
            if (lidSwitch >= 0) {
                note(lidSwitch != 0 ? "lid switch open" : "lid switch closed",
                     hingeAngle);
            }
        }

        // ---- display mode and capture health --------------------------------
        const uint32_t metricsWidth =
            static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN));
        const uint32_t metricsHeight =
            static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN));
        if (metricsWidth != 0 && metricsHeight != 0 &&
            (metricsWidth != width || metricsHeight != height)) {
            // The desktop moved to another output or resolution, so everything
            // in hand is sized for the old mode: drop it and rebuild.
            overlay.Show(false);
            hasContent = false;
            capture.Stop();
            ++modeChanges;
            safety.Reset();
        }

        // A suspend, a display power transition or a mode change all mean the
        // frame in hand may be arbitrarily old.
        if (overlay.ConsumeEnvironmentChange()) {
            safety.Reset();
        }

        const bool ready = safety.Update(lidClosedLatch, overlay.DisplayOn(),
                                         capture.Healthy(), sensorFresh,
                                         sample.steadySeconds);
        if (safety.Current() != lastGateState) {
            lastGateState = safety.Current();
            note(DisplaySafetyGate::Text(lastGateState), hingeAngle);
        }

        // ---- capture recovery -----------------------------------------------
        // A duplication that came back DXGI_ERROR_ACCESS_LOST is dead and can
        // only be replaced; retry at most once a second so a panel that is still
        // off cannot be hammered.
        if (!capture.Healthy() &&
            sample.steadySeconds - lastRecoverySeconds >= 1.0) {
            lastRecoverySeconds = sample.steadySeconds;
            if (capture.TryRestart(device.Device())) {
                terminal.Write("\ncapture restarted at " +
                               std::to_string(capture.Width()) + "x" +
                               std::to_string(capture.Height()) + "\n");
            }
        }

        // The replacement can report a different mode than the chain was built
        // for; follow it rather than drawing a mis-sized frame.
        if (capture.Healthy() && capture.Width() != 0 && capture.Height() != 0 &&
            (capture.Width() != width || capture.Height() != height)) {
            const uint32_t newWidth = capture.Width();
            const uint32_t newHeight = capture.Height();
            overlay.Show(false);
            hasContent = false;
            overlay.Resize(newWidth, newHeight);
            device.Resize(newWidth, newHeight);
            renderer.Resize(newWidth, newHeight);
            if (!CreateContentTexture(device.Device(), newWidth, newHeight, content)) {
                terminal.Write(
                    "\nERROR: could not resize the content texture; stopping.\n");
                break;
            }
            width = newWidth;
            height = newHeight;
            parameters.eyeDistancePx = static_cast<float>(width) * 6.4f;
            safety.Reset();
        }

        // Past flat the pose cannot be resolved with a single IMU, so the effect
        // switches itself off and the desktop comes back untouched.
        const bool effectWanted = ready && !pastFlat && angleDelta > 0.05 &&
                                  tracker.Valid();

        // ---- snapshot policy, the single most important part of this loop ----
        //
        // duo-open's whole overlay design rests on this line from its header:
        // "Opening: the inner panel comes up -> ONE screenshot -> a full-screen
        //  overlay draws it through the fold shader."
        //
        // Exactly one capture per fold, taken with the overlay hidden, then the
        // image is frozen for the duration.  Capturing every frame would feed
        // the overlay's own output back into the shader: the desktop duplication
        // API captures the composed screen, so the shader would sample its last
        // result, and each frame would come out blurrier than the last until the
        // whole screen went black.
        if (!effectWanted) {
            // Fail closed: nothing is drawn while the environment is not
            // trustworthy.  The frame in hand is NOT discarded though -- a
            // timeout from the duplication means it still matches the desktop,
            // and keeping it is what lets the effect come back the instant the
            // lid is open instead of after a fresh capture.
            overlay.Show(false);
            if (lastEffectWanted) {
                note("effect off", hingeAngle);
            }
            lastEffectWanted = false;

            // Opportunistic refresh, at most once a second: whenever something
            // moved on screen while the overlay was hidden, a newer frame lands
            // in the content texture, so the next fold starts from a current
            // picture rather than from the one taken before the lid was closed.
            if (sample.steadySeconds - lastRefreshSeconds >= 1.0) {
                lastRefreshSeconds = sample.steadySeconds;
                if (capture.Healthy() && capture.AcquireFrame(0)) {
                    capture.CopyFrameTo(device.Context(), content.Get());
                    capture.ReleaseFrame();
                    ++captureCount;
                    hasContent = true;
                }
            }
        } else if (!lastEffectWanted) {
            // Hide first: a capture taken while the overlay is up would contain
            // the overlay itself.
            overlay.Show(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            hasContent = grabDesktop();
            lastEffectWanted = true;
            if (effectWanted) {
                note("effect on", hingeAngle);
            }
        } else if (!hasContent) {
            // An earlier attempt came back empty; keep asking while the effect
            // wants to be visible.
            hasContent = grabDesktop();
        }

        // Nothing is drawn without a frame that was captured for this pass: a
        // stale texture is worse than no effect at all.
        if (effectWanted && hasContent) {
            overlay.Show(true);
            // Another topmost window can steal the front slot; re-assert it
            // periodically so the effect cannot end up hidden behind something.
            if ((++loopCount % 120) == 0) {
                overlay.BringToFront();
            }
        }
        // Read the real window state rather than our own bookkeeping: if the
        // system refuses to show the window, the status line must say so.
        const bool overlayVisible = IsWindowVisible(overlay.Handle()) != FALSE;

        if (effectWanted && hasContent) {
            device.BeginFrame(0.0f, 0.0f, 0.0f, 1.0f);
            renderer.Render(device.Context(), content.Get(),
                            device.BackBuffer(),
                            static_cast<float>(angleDelta), parameters);

            // Dump before Present: with a DISCARD swap chain the back buffer
            // contents are undefined once it has been presented.
            if (options.dumpFrames &&
                (renderedFrames == 0 || renderedFrames == 30 || renderedFrames == 120)) {
                char path[128] = {};
                std::snprintf(path, sizeof(path), "fold-dump-backbuffer-%llu.bmp",
                              static_cast<unsigned long long>(renderedFrames));
                if (DumpTextureToBmp(device.Device(), device.Context(),
                                     device.BackBufferTexture(), path)) {
                    terminal.Write(std::string("\ndumped ") + path + "\n");
                }
            }

            device.Present(true);
            ++renderedFrames;
        }

        // ---- status line --------------------------------------------------
        if (sample.steadySeconds - lastReportSeconds >= 0.25) {
            lastReportSeconds = sample.steadySeconds;
            std::string line =
                "\r" + FormatLine(effectWanted ? "FOLD " : "idle ", hingeAngle,
                                  angleDelta, options.activationAngleDeg,
                                  DisplaySafetyGate::Text(safety.Current()),
                                  lidMode, overlayVisible, hasContent,
                                  renderedFrames, captureCount);
            if (line.size() < lastLineLength) {
                line += std::string(lastLineLength - line.size(), ' ');
            }
            lastLineLength = line.size();
            terminal.Write(line);
        }

        if (options.seconds > 0.0 && sample.steadySeconds >= options.seconds) {
            break;
        }

        // While the effect is off there is nothing to draw, so the loop drops to
        // a low rate and idles instead of spinning.
        const double rate = effectWanted ? options.rateHz : 20.0;
        const auto period =
            std::chrono::duration<double>(1.0 / std::max(1.0, rate));
        nextTick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
        const auto now = std::chrono::steady_clock::now();
        if (nextTick < now) {
            nextTick = now;
        }
        std::this_thread::sleep_until(nextTick);
    }

    // ---- shutdown ---------------------------------------------------------
    if (options.keepDisplayAwake) {
        // Hand the display's idle timeout back to the system.
        SetThreadExecutionState(ES_CONTINUOUS);
    }
    overlay.Show(false);
    capture.Stop();
    renderer.Destroy();
    device.Destroy();
    overlay.Destroy();

    terminal.Write("\nFold effect stopped. Frames rendered: " +
                   std::to_string(renderedFrames) + ", captures: " +
                   std::to_string(captureCount) + ", mode changes: " +
                   std::to_string(modeChanges) + "\n");
    return 0;
}

} // namespace dragonfly
