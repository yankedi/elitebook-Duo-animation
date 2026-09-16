// ---------------------------------------------------------------------------
//  FoldEffectMode.cpp
// ---------------------------------------------------------------------------
#include "FoldEffectMode.h"

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

std::string FormatLine(const char* phase, double hingeAngle, double progress,
                       double tiltDegrees, int lidMode, bool overlayVisible,
                       bool hasContent, uint64_t frames, uint64_t captures) {
    char buffer[300];
    std::snprintf(buffer, sizeof(buffer),
                  "%s  hinge %6.1f   progress %5.3f   tilt %5.1f   lid %s   "
                  "overlay %s   content %-3s   frames %llu   grabs %llu",
                  phase, hingeAngle, progress, tiltDegrees,
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
    // These are the reference project's numbers, kept rather than re-derived.
    //
    // Duo-animation ships eyeDistance = 450 mm against a 70 mm panel: that is
    // 6.4x the panel width, and the ratio is what matters.  It holds the
    // projection magnification
    //     t = eyeDistance / (eyeDistance - gap)
    // near 1, so the effect reads as frosted glass rather than as a magnified
    // image.  Using the 450 mm literally on a 300 mm-wide laptop panel gives
    // 1.1x width instead of 6.4x, magnification climbs past 1.5, and a third of
    // the frame is projected off the plane and comes back black.
    FoldEffectParameters parameters;
    parameters.blurSpread = options.blurSpread;
    parameters.darkening = options.darkening;
    parameters.maxTiltDegrees = options.maxTiltDegrees;
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

    terminal.Write(
        "\nFold effect armed.\n"
        "The overlay appears only while the lid is moving and is removed once the\n"
        "effect resolves, so a settled desktop is never covered.\n"
        "\n"
        "STOP: press [ESC] or [F10] -- these are read globally, so they work even\n"
        "      though the overlay holds the screen.  [Q] in this console also works.\n\n");
    char setup[400];
    std::snprintf(setup, sizeof(setup),
                  "  display %ux%u, %u dpi -> eye distance %.0f px, blur %.3f, darken %.3f\n"
                  "  capture %ux%u, DXGI_FORMAT %d, overlay %ux%u\n\n",
                  width, height, dpi, parameters.eyeDistancePx,
                  parameters.blurSpread, parameters.darkening,
                  capture.Width(), capture.Height(), static_cast<int>(capture.Format()),
                  width, height);
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

    // ---- sensor state -----------------------------------------------------
    OrientationTracker tracker;
    tracker.Configure(0.075);

    bool pastFlat = false;
    double lastSampleSeconds = 0.0;
    double lastReportSeconds = -1.0;
    bool lastEffectWanted = false;
    uint64_t renderedFrames = 0;
    uint64_t captureCount = 0;
    uint64_t loopCount = 0;

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
        const double progress =
            pastFlat ? 1.0 : std::clamp(hingeAngle / 180.0, 0.0, 1.0);

        // ---- decide whether the effect should be on screen ----------------
        // Past flat the pose cannot be resolved with a single IMU, so the effect
        // switches itself off and the desktop comes back untouched.
        const bool effectWanted = progress < 0.999 && tracker.Valid();

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
        if (effectWanted && !lastEffectWanted) {
            // Hide first: a capture taken while the overlay is up would contain
            // the overlay itself.
            overlay.Show(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            for (int attempt = 0; attempt < 3; ++attempt) {
                if (capture.AcquireFrame(150)) {
                    capture.CopyFrameTo(device.Context(), content.Get());
                    capture.ReleaseFrame();
                    hasContent = true;
                    ++captureCount;
                    break;
                }
            }
        }
        lastEffectWanted = effectWanted;

        overlay.Show(effectWanted);
        // Another topmost window can steal the front slot; re-assert it
        // periodically so the effect cannot end up hidden behind something.
        if (effectWanted && (++loopCount % 120) == 0) {
            overlay.BringToFront();
        }
        // Read the real window state rather than our own bookkeeping: if the
        // system refuses to show the window, the status line must say so.
        const bool overlayVisible = IsWindowVisible(overlay.Handle()) != FALSE;

        if (effectWanted && hasContent) {
            device.BeginFrame(0.0f, 0.0f, 0.0f, 1.0f);
            renderer.Render(device.Context(), content.Get(),
                            device.BackBuffer(),
                            static_cast<float>(hingeAngle), parameters);

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
            const double tiltShown =
                FoldRenderer::TiltDegreesForHinge(static_cast<float>(hingeAngle),
                                                  parameters);
            std::string line = "\r" + FormatLine(effectWanted ? "FOLD " : "idle ",
                                                 hingeAngle, progress, tiltShown,
                                                 lidMode, overlayVisible,
                                                 hasContent, renderedFrames,
                                                 captureCount);
            if (line.size() < 140) {
                line += std::string(140 - line.size(), ' ');
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
