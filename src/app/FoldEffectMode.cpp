// ---------------------------------------------------------------------------
//  FoldEffectMode.cpp
// ---------------------------------------------------------------------------
#include "FoldEffectMode.h"

#include "../capture/DesktopCapture.h"
#include "../graphics/D3DDevice.h"
#include "../graphics/FoldRenderer.h"
#include "../graphics/OverlayWindow.h"
#include "../orientation/OrientationTracker.h"
#include "../sensors/CustomSensorManager.h"
#include "../sensors/SensorManager.h"

#include <windows.h>

#include <wrl/client.h>

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

std::string FormatLine(const char* phase, double hingeAngle, double progress,
                       double tiltDegrees, int lidMode, bool overlayVisible) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "%s  hinge %6.1f   progress %5.3f   tilt %5.1f   lid %s   overlay %s",
                  phase, hingeAngle, progress, tiltDegrees,
                  lidMode < 0 ? "--" : std::to_string(lidMode).c_str(),
                  overlayVisible ? "ON " : "off");
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
    const uint32_t width = overlay.Width();
    const uint32_t height = overlay.Height();

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

    // ---- fold shader ------------------------------------------------------
    FoldEffectParameters parameters;
    parameters.blurSpread = options.blurSpread;
    parameters.darkening = options.darkening;
    parameters.maxTiltDegrees = options.maxTiltDegrees;

    // Eye distance from millimetres, using this display's own density, exactly
    // as the reference implementation does.
    const UINT dpi = GetDpiForWindow(overlay.Handle());
    const float pixelsPerMm =
        ((dpi > 0 ? static_cast<float>(dpi) : 96.0f) / 25.4f);
    parameters.eyeDistancePx = options.eyeDistanceMm * pixelsPerMm;

    FoldRenderer renderer;
    if (!renderer.Create(device.Device(), width, height)) {
        terminal.Write(std::string("ERROR: fold shader failed: ") +
                       renderer.LastError() + "\n");
        capture.Stop();
        overlay.Destroy();
        return 1;
    }

    terminal.Write(
        "\nFold effect armed.\n"
        "The overlay appears only while the lid is moving and is removed once the\n"
        "effect resolves, so a settled desktop is never covered.\n"
        "Console keys: [Q] quit.\n\n");
    char setup[320];
    std::snprintf(setup, sizeof(setup),
                  "  display %ux%u, %u dpi -> eye distance %.0f px, blur %.3f, darken %.3f\n\n",
                  width, height, dpi, parameters.eyeDistancePx,
                  parameters.blurSpread, parameters.darkening);
    terminal.Write(setup);

    // ---- prime the first frame -------------------------------------------
    bool hasContent = false;
    if (capture.AcquireFrame(1000)) {
        capture.CopyFrameTo(device.Context(), content.Get());
        capture.ReleaseFrame();
        hasContent = true;
    } else {
        terminal.Write("WARNING: no desktop frame captured yet; the effect will "
                       "fill in as soon as the screen changes.\n");
    }

    // ---- sensor state -----------------------------------------------------
    OrientationTracker tracker;
    tracker.Configure(0.075);

    bool pastFlat = false;
    bool overlayVisible = false;
    double lastSampleSeconds = 0.0;
    double lastReportSeconds = -1.0;
    uint64_t renderedFrames = 0;

    auto nextTick = std::chrono::steady_clock::now();

    while (!stopFlag.load()) {
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
        const double progress =
            pastFlat ? 1.0 : std::clamp(hingeAngle / 180.0, 0.0, 1.0);

        // ---- decide whether the effect should be on screen ----------------
        // Past flat the pose cannot be resolved with a single IMU, so the effect
        // switches itself off and the desktop comes back untouched.
        const bool effectWanted = progress < 0.999 && tracker.Valid();
        overlay.Show(effectWanted);
        overlayVisible = overlay.IsShown();

        if (effectWanted) {
            // Non-blocking: a timeout simply means the desktop did not change,
            // in which case the previous contents are still correct.
            if (capture.AcquireFrame(0)) {
                capture.CopyFrameTo(device.Context(), content.Get());
                capture.ReleaseFrame();
                hasContent = true;
            }

            if (hasContent) {
                device.BeginFrame(0.0f, 0.0f, 0.0f, 1.0f);
                renderer.Render(device.Context(), content.Get(),
                                device.BackBuffer(),
                                static_cast<float>(hingeAngle), parameters);
                device.Present(true);
                ++renderedFrames;
            }
        }

        // ---- status line --------------------------------------------------
        if (sample.steadySeconds - lastReportSeconds >= 0.25) {
            lastReportSeconds = sample.steadySeconds;
            const double tiltShown =
                FoldRenderer::TiltDegreesForHinge(static_cast<float>(hingeAngle),
                                                  parameters);
            std::string line = "\r" + FormatLine(effectWanted ? "FOLD " : "idle ",
                                                 hingeAngle, progress, tiltShown,
                                                 lidMode, overlayVisible);
            if (line.size() < 118) {
                line += std::string(118 - line.size(), ' ');
            }
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
    overlay.Show(false);
    capture.Stop();
    renderer.Destroy();
    device.Destroy();
    overlay.Destroy();

    terminal.Write("\nFold effect stopped. Frames rendered: " +
                   std::to_string(renderedFrames) + "\n");
    return 0;
}

} // namespace dragonfly
