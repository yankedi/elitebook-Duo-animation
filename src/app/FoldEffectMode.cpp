// ---------------------------------------------------------------------------
//  FoldEffectMode.cpp
// ---------------------------------------------------------------------------
#include "FoldEffectMode.h"

#include "DisplaySafetyGate.h"

#include "../capture/DesktopCapture.h"
#include "../graphics/CursorRenderer.h"
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
//
// A full mip chain is allocated because the blur samples it: a prefiltered
// level is what makes a 60 px radius smooth instead of speckled, and it costs
// one sample rather than a wide hand-written kernel.
bool CreateContentTexture(ID3D11Device* device, uint32_t width, uint32_t height,
                          Microsoft::WRL::ComPtr<ID3D11Texture2D>& out) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 0;  // full chain, filled in by GenerateMips
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // matches the duplication output
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    return SUCCEEDED(device->CreateTexture2D(&description, nullptr, &out));
}

// ---------------------------------------------------------------------------
//  Physical panel geometry
//
//  The projection needs distances in *pixels*, and the conversion is the panel's
//  own scale: the desktop covers the panel, so px/mm = desktop width / panel
//  width.  Windows' reported DPI is the logical one -- it is what display
//  scaling changes -- so it cannot be used for this.
// ---------------------------------------------------------------------------
BOOL CALLBACK FindPrimaryMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) != FALSE &&
        (info.dwFlags & MONITORINFOF_PRIMARY) != 0) {
        *reinterpret_cast<std::wstring*>(context) = info.szDevice;
        return FALSE;  // found it, stop enumerating
    }
    return TRUE;
}

// Physical width of the primary panel in millimetres, from its EDID.  Returns 0
// when the EDID cannot be read, and the caller falls back to a nominal panel.
double PrimaryPanelWidthMm() {
    std::wstring adapter;
    EnumDisplayMonitors(nullptr, nullptr, FindPrimaryMonitor,
                        reinterpret_cast<LPARAM>(&adapter));
    if (adapter.empty()) {
        return 0.0;
    }

    DISPLAY_DEVICEW monitor{};
    monitor.cb = sizeof(monitor);
    if (EnumDisplayDevicesW(adapter.c_str(), 0, &monitor, 0) == FALSE) {
        return 0.0;
    }

    // monitor.DeviceID looks like MONITOR\AUO1234\{GUID}\0001, which is also its
    // path under Enum in the registry.
    const std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\" +
                             std::wstring(monitor.DeviceID) + L"\\Device Parameters";
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &handle) !=
        ERROR_SUCCESS) {
        return 0.0;
    }

    BYTE edid[256] = {};
    DWORD size = sizeof(edid);
    DWORD type = 0;
    const LSTATUS status =
        RegQueryValueExW(handle, L"EDID", nullptr, &type, edid, &size);
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS || size < 24) {
        return 0.0;
    }

    // EDID bytes 21 and 22 hold the image size in centimetres.
    const uint32_t widthMm = static_cast<uint32_t>(edid[21]) * 10u;
    if (widthMm < 50 || widthMm > 2000) {
        return 0.0;
    }
    return static_cast<double>(widthMm);
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

// Anchor motion filter, taken from the reference: a dead band measured from the
// last accepted angle (small shakes are ignored, slow movement still
// accumulates), a pause before the anchor moves, and an ease rather than a jump.
constexpr double kAnchorJitterTolerance = 0.25;  // degrees
constexpr double kAnchorDelaySeconds = 0.2;      // stillness before re-anchoring
constexpr double kAnchorEaseSeconds = 0.12;      // time constant of the ease

std::string FormatLine(const char* phase, double hingeAngle, double angleDelta,
                       double anchorAngle, const char* gateText, int lidMode,
                       bool overlayVisible, bool hasContent, uint64_t frames,
                       uint64_t captures) {
    char buffer[300];
    std::snprintf(buffer, sizeof(buffer),
                  "%s  hinge %6.1f   delta %6.1f   anchor %6.1f   %-24s lid %s   "
                  "overlay %s   content %-3s   frames %llu   grabs %llu",
                  phase, hingeAngle, angleDelta, anchorAngle, gateText,
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
    if (!overlay.Create(L"Dragonfly Fold Effect", options.layeredOverlay)) {
        terminal.Write("ERROR: could not create the overlay window.\n");
        return 1;
    }
    uint32_t width = overlay.Width();
    uint32_t height = overlay.Height();
    // Emitted into this function; reassigned if the capture reports a new mode.
    uint32_t contentWidth = 0;
    uint32_t contentHeight = 0;

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

    // ---- the viewer --------------------------------------------------------
    // The reference gives the eye in screen heights -- (0, 0.65, 1.6) -- and its
    // look is calibrated around that, so it is the default.  A real distance in
    // millimetres can be given instead; the panel's physical size comes from its
    // EDID, because the DPI Windows reports is the logical one.
    // The geometry is worked out in *desktop* pixels, because that is what the
    // captured content is in and what the pane is a viewport onto.
    contentWidth = capture.Width();
    contentHeight = capture.Height();

    const double panelWidthMm = PrimaryPanelWidthMm();
    const double panelHeightMm =
        (panelWidthMm > 0.0) ? (panelWidthMm * static_cast<double>(contentHeight) /
                                static_cast<double>(contentWidth))
                             : 165.0;  // nominal 13.3" 16:9
    const double eyeHeights = (options.eyeDistanceMm > 0.0f)
                                  ? (options.eyeDistanceMm / panelHeightMm)
                                  : options.eyeDistanceHeights;
    parameters.eyeDistancePx = static_cast<float>(eyeHeights * contentHeight);
    parameters.eyeUpPx =
        static_cast<float>(options.eyeHeightHeights * contentHeight);
    // A floor for the edge feather, so the picture's boundary is never razor
    // sharp even where the pane is clear; the blur adds its own width on top.
    parameters.edgeFadePx = 2.0f;
    // The picture hangs on the pane's anchored plane when this is zero; see
    // FoldEffectParameters::screenDepthPx.
    parameters.screenDepthPx =
        static_cast<float>(options.screenDepthRatio * parameters.eyeDistancePx);
    parameters.pictureScale = options.pictureScale;
    // Where the pane sits on the desktop: it covers the work area, so the
    // taskbar keeps its own backdrop and its acrylic stays cached.
    parameters.windowWidth = static_cast<float>(width);
    parameters.windowHeight = static_cast<float>(height);
    parameters.originX = static_cast<float>(overlay.OriginX());
    parameters.originY = static_cast<float>(overlay.OriginY());

    switch (options.glassPreset) {
    case FoldEffectOptions::GlassPreset::Reference:
        // Exactly the reference's look: bright picture, dark void, no tinting.
        parameters.darkening = 0.0f;
        parameters.sheenStrength = 0.0f;
        parameters.edgeGlow = 0.0f;
        parameters.dispersionPx = 0.0f;
        parameters.scatterDesaturation = 0.0f;
        break;
    case FoldEffectOptions::GlassPreset::Glass:
        // The reference plus this project's glass cues.
        parameters.darkening = 0.006f;
        break;
    case FoldEffectOptions::GlassPreset::Plain:
        // The model as it was before: picture glued to the panel, no cues.
        parameters.parallax = 0.0f;
        parameters.darkening = 0.015f;
        parameters.sheenStrength = 0.0f;
        parameters.edgeGlow = 0.0f;
        parameters.dispersionPx = 0.0f;
        parameters.scatterDesaturation = 0.0f;
        parameters.attenuationFloor = 0.35f;
        break;
    }

    // ---- material overrides, applied after the preset ----------------------
    if (options.blurOverride >= 0.0f) {
        parameters.blurStrength = options.blurOverride;
    }
    if (options.dispersionOverride >= 0.0f) {
        parameters.dispersionPx = options.dispersionOverride;
    }
    if (options.sheenOverride >= 0.0f) {
        parameters.sheenStrength = options.sheenOverride;
    }
    if (options.edgeOverride >= 0.0f) {
        parameters.edgeGlow = options.edgeOverride;
    }
    if (options.parallaxOverride >= 0.0f) {
        parameters.parallax = options.parallaxOverride;
    }

    const UINT dpi = GetDpiForWindow(overlay.Handle());

    FoldRenderer renderer;
    // The renderer is fed the *content* size, not the pane's: the shader works
    // in desktop pixels so that the picture's geometry matches the captured
    // frame, and the pane is only a viewport onto it.
    contentWidth = capture.Width();
    contentHeight = capture.Height();
    if (!renderer.Create(device.Device(), contentWidth, contentHeight)) {
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

    // Execution-state requests.  Held with ES_CONTINUOUS so they stay in force
    // between calls, and re-issued only when the combination actually changes.
    //   - the panel is kept powered while the effect runs, so the idle timeout
    //     never blanks it and there is nothing to wake up;
    //   - the system is kept out of Modern Standby while the lid is shut (opt
    //     in), because a resume costs far more than a panel power-on.
    bool displayHeld = false;
    bool systemHeld = false;
    auto applyPowerRequests = [&](bool lidClosed) {
        const bool wantDisplay = options.keepDisplayAwake;
        const bool wantSystem = options.keepSystemAwake && lidClosed;
        if (wantDisplay == displayHeld && wantSystem == systemHeld) {
            return;
        }
        displayHeld = wantDisplay;
        systemHeld = wantSystem;
        DWORD flags = ES_CONTINUOUS;
        if (displayHeld) {
            flags |= ES_DISPLAY_REQUIRED;
        }
        if (systemHeld) {
            flags |= ES_SYSTEM_REQUIRED;
        }
        SetThreadExecutionState(flags);
    };

    // ---- cursor ------------------------------------------------------------
    // The captured desktop has no pointer in it, so it is drawn back in before
    // the pane is rendered; otherwise the effect leaves a sharp cursor floating
    // in front of the glass.
    CursorRenderer cursor;
    if (!cursor.Create(device.Device())) {
        terminal.Write(std::string("WARNING: the cursor will not be part of the "
                                   "effect: ") + cursor.LastError() + "\n");
    }

    // ---- live capture ------------------------------------------------------
    // Ask DWM to keep the overlay out of screen capture.  When that works the
    // duplication can be read *every frame*, so the picture stays live: video
    // keeps playing, windows keep updating, and the user is looking at their
    // real desktop through a pane rather than at a photograph of it.  Without
    // it (pre-2004 Windows) reading the desktop would read our own output back,
    // and the effect has to freeze on one snapshot per fold instead.
    const bool liveCapture = options.excludeFromCapture && overlay.ExcludeFromCapture();

    terminal.Write(
        "\nFold effect armed.\n"
        "Move the lid and the picture stays where it was while the panel turns\n"
        "away from it, blurring as the gap opens; hold still and the picture\n"
        "eases back to wherever the lid is resting.  Above that the desktop is\n"
        "untouched.\n"
        "\n"
        "STOP: press [ESC] or [F10] -- these are read globally, so they work even\n"
        "      though the overlay holds the screen.  [Q] in this console also works.\n\n");
    char setup[600];
    const int initialLidSwitch = overlay.LidSwitchState();
    std::snprintf(setup, sizeof(setup),
                  "  display %ux%u, %u dpi -> eye distance %.0f px\n"
                  "  activation %.0f deg, blur %.0f/1000px, darken %.3f, max delta %.0f deg\n"
                  "  glass %s (sheen %.2f, edge %.2f, dispersion %.1f px)\n"
                  "  panel %.0f mm wide, eye %.0f px (%.2f screen heights), parallax %.2f\n"
                  "  capture %ux%u, DXGI_FORMAT %d, overlay %ux%u\n"
                  "  monitor power %s (notify %s), lid switch %s (notify %s), settle %.2f s\n"
                  "  capture %s, display %s, reads %.0f/s, swap %s\n\n",
                  width, height, dpi, parameters.eyeDistancePx,
                  options.activationAngleDeg, parameters.blurStrength,
                  parameters.darkening, parameters.maxDeltaDegrees,
                  options.glassPreset == FoldEffectOptions::GlassPreset::Reference
                      ? "reference"
                      : (options.glassPreset == FoldEffectOptions::GlassPreset::Plain
                             ? "plain"
                             : "glass"),
                  parameters.sheenStrength, parameters.edgeGlow,
                  parameters.dispersionPx,
                  panelWidthMm > 0.0 ? panelWidthMm : 294.0,
                  parameters.eyeDistancePx, eyeHeights, parameters.parallax,
                  capture.Width(), capture.Height(), static_cast<int>(capture.Format()),
                  width, height, overlay.DisplayOn() ? "on" : "off",
                  overlay.DisplayNotifyActive() ? "yes" : "NO",
                  initialLidSwitch < 0 ? "unknown"
                                       : (initialLidSwitch != 0 ? "open" : "closed"),
                  overlay.LidNotifyActive() ? "yes" : "NO", safety.RecoveryDelay(),
                  liveCapture ? "live (overlay excluded from capture)"
                              : "snapshot per fold (no capture exclusion)",
                  options.keepDisplayAwake ? "kept awake" : "system default",
                  options.captureRateHz, device.FlipModel() ? "flip" : "bitblt");
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

    // The anchor the picture is held at, and the motion filter that keeps it
    // there.  See the loop below; the numbers are the reference's.
    double anchorAngle = 0.0;
    double lastAcceptedHinge = 0.0;
    double stableSeconds = 0.0;
    bool anchorValid = false;

    bool pastFlat = false;
    // Latched, because the hinge estimate is driven by acos() of the panel
    // normal: near the fully closed pose that value sits on the singularity and
    // is far too noisy to be compared against a single threshold every frame.
    bool lidClosedLatch = false;
    double lastSampleSeconds = 0.0;
    double lastReportSeconds = -1.0;
    double lastRecoverySeconds = -1.0;
    double lastRefreshSeconds = 0.0;
    double lastLiveCaptureSeconds = 0.0;
    // The angle last presented, so a frame is only presented when the picture
    // actually moved.  A sentinel when nothing is on screen yet.
    double lastPresentedDelta = 1.0e9;
    // When the content texture last received a real desktop frame.  A snapshot
    // that is much older than this stops counting as drawable.
    double lastCaptureSeconds = SteadySeconds();
    std::size_t lastLineLength = 0;
    bool lastEffectWanted = false;
    uint64_t renderedFrames = 0;
    uint64_t captureCount = 0;
    uint64_t loopCount = 0;
    uint64_t modeChanges = 0;
    // The priming capture below has already written the content texture, so the
    // chain starts out dirty; every copy sets it again.
    bool contentDirty = true;

    // Environment bookkeeping, for the transition log and the gate.
    DisplaySafetyGate::State lastGateState = DisplaySafetyGate::State::Recovering;
    int lastLidSwitch = overlay.LidSwitchState();
    bool lastDisplayOn = overlay.DisplayOn();
    unsigned lastInputMask = 0;

    // Puts the pointer into the content texture.  The duplication reports it
    // next to the frame rather than inside it, so it has to be drawn in before
    // the mip chain is rebuilt and the pane is rendered.
    auto compositePointer = [&]() {
        const DesktopPointer& pointer = capture.Pointer();
        if (!cursor.Ready() || !pointer.visible) {
            return;
        }
        cursor.UpdateShape(pointer);
        cursor.Draw(device.Context(), content.Get(), pointer);
    };

    // Grabs the desktop into the content texture.
    //
    // A timeout is NOT a failure here.  AcquireNextFrame only hands back a frame
    // when the composited desktop changed, and it times out when the image is
    // identical to the one it delivered last -- and that delivered image is
    // exactly what the content texture holds.  So "no new frame" means the frame
    // in hand is still the current desktop, and it can be used as is.
    //
    // The duplication being dead is no longer treated as fatal to the pass: the
    // texture still holds the last real desktop, and the desktop is static
    // whenever the panel is off -- which is exactly when the duplication dies.
    // The caller bounds how old that snapshot may be.
    auto grabDesktop = [&]() -> bool {
        // A frame is only worth waiting for when there is nothing in hand; if
        // there is, one non-blocking look is enough, and a timeout proves the
        // held frame still matches.  Waiting here would stall the render loop
        // right at the start of a fold, which is the worst possible moment.
        const uint32_t timeoutMs = hasContent ? 0u : 120u;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (capture.AcquireFrame(timeoutMs)) {
                capture.CopyFrameTo(device.Context(), content.Get());
                capture.ReleaseFrame();
                ++captureCount;
                hasContent = true;
                contentDirty = true;
                lastCaptureSeconds = SteadySeconds();
                compositePointer();
                return true;
            }
            if (!capture.Healthy()) {
                return hasContent;
            }
        }
        // Nothing changed on screen, so what is in hand is still current.
        return hasContent;
    };

    // A snapshot stops being usable when it is old enough that it may no longer
    // describe the desktop; see FoldEffectOptions::contentMaxAgeSeconds.
    auto snapshotUsable = [&]() -> bool {
        return hasContent &&
               (SteadySeconds() - lastCaptureSeconds) <= options.contentMaxAgeSeconds;
    };

    // A small event log, so a run can be read back afterwards: the whole point
    // of the safety gate is timing, and the console status line is overwritten
    // every 0.25 s.  Timestamps come along because durations are what matters.
    auto note = [&](const char* what, double hingeDeg) {
        char buffer[200];
        std::snprintf(buffer, sizeof(buffer), "\n  [env] %-22s t %7.2f  hinge %6.1f\n",
                      what, SteadySeconds(), hingeDeg);
        terminal.Write(buffer);
    };

    // The system cursor is drawn above every window, so it would sit sharp in
    // front of the pane; while the effect is on it is hidden and the pointer is
    // part of the picture instead.  Shown once at startup as well, in case a
    // previous run died while it was hidden.
    ShowCursor(TRUE);
    bool systemCursorHidden = false;
    auto showSystemCursor = [&](bool show) {
        if (show != systemCursorHidden) {
            return;
        }
        systemCursorHidden = !show;
        ShowCursor(show ? TRUE : FALSE);
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

        // ---- the anchor: where the picture stays ----------------------------
        // Not a fixed angle.  The picture is anchored to the pose the lid was in
        // when it stopped moving, because that is the pose the user was looking
        // at: close the lid from 120 degrees and the desktop stays at 120
        // degrees, blurring, while the panel swings away from it.  Once the lid
        // holds still again the anchor eases over to the new pose, which is what
        // makes the effect end by itself instead of sitting there frozen.
        //
        // This is lid-plane's AutoAnchor, and its numbers: a dead band measured
        // from the last accepted angle, and an ease rather than a jump.
        if (!anchorValid) {
            anchorAngle = hingeAngle;
            lastAcceptedHinge = hingeAngle;
            anchorValid = true;
        }

        if (std::abs(hingeAngle - lastAcceptedHinge) > kAnchorJitterTolerance) {
            lastAcceptedHinge = hingeAngle;
            stableSeconds = 0.0;
        } else {
            stableSeconds += dt;
        }

        if (stableSeconds >= kAnchorDelaySeconds && !lidClosedLatch) {
            const double ease = 1.0 - std::exp(-dt / kAnchorEaseSeconds);
            anchorAngle += (hingeAngle - anchorAngle) * ease;
        }

        // Signed: closing swings the pane one way and opening the other, and
        // both are rendered.
        const double angleDelta = anchorAngle - hingeAngle;

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
        applyPowerRequests(lidClosedLatch);

        // ---- environment transition log --------------------------------------
        // The gate is all about timing, and the 0.25 s status line is
        // overwritten in place, so the transitions get their own lines: this is
        // the record that says whether a late effect came from the panel waking
        // up or from something this program did.
        const bool displayOn = overlay.DisplayOn();

        // The four raw inputs, whenever any of them changes.  Sensor age is the
        // important one: a fusion sensor simply stops publishing while the
        // machine is still, so "no reading for a while" means "nothing is
        // moving", not "broken" -- and it also bounds how early the effect can
        // possibly know where the lid is.
        const unsigned inputMask = (sensorFresh ? 1u : 0u) |
                                   (lidSwitch == 0 ? 2u : 0u) |
                                   (displayOn ? 4u : 0u) |
                                   (capture.Healthy() ? 8u : 0u);
        if (inputMask != lastInputMask) {
            lastInputMask = inputMask;
            char buffer[240];
            std::snprintf(buffer, sizeof(buffer),
                          "\n  [in ] t %7.2f  hinge %6.1f  sensorAge %6.2f s  "
                          "lidSwitch %2d  display %-3s  capture %-4s\n",
                          SteadySeconds(), hingeAngle, sensorAge, lidSwitch,
                          displayOn ? "on" : "OFF",
                          capture.Healthy() ? "ok" : "LOST");
            terminal.Write(buffer);
        }

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
                                         capture.Healthy() || snapshotUsable(),
                                         sensorFresh, sample.steadySeconds);
        if (safety.Current() != lastGateState) {
            lastGateState = safety.Current();
            note(DisplaySafetyGate::Text(lastGateState), hingeAngle);
        }

        // ---- the effect belongs to the act of closing ----------------------
        // (see the header: keyed to how far below the activation angle the lid
        // has come, not to the absolute hinge angle)
        //
        // Past flat the pose cannot be resolved with a single IMU, so the effect
        // switches itself off and the desktop comes back untouched.  The
        // threshold is asymmetric on purpose: right at the activation angle the
        // estimate wanders by about a degree, and a single threshold makes the
        // overlay pop on and off several times while the lid settles.
        // The threshold is asymmetric on purpose: right at the anchor the
        // estimate wanders by about a degree, and a single threshold makes the
        // overlay pop on and off while the lid settles.  Going on takes a clear
        // delta, going off takes the lid actually being back at the anchor.
        // Signed, so closing and opening both show the pane moving.
        const bool preview = options.previewDeltaDegrees > 0.0;
        const double deltaThreshold = lastEffectWanted ? -0.3 : 0.6;
        const bool effectWanted =
            ready && tracker.Valid() &&
            (preview ? true
                     : (!pastFlat && std::abs(angleDelta) > deltaThreshold));
        const double renderedDelta =
            preview ? options.previewDeltaDegrees : angleDelta;

        // ---- housekeeping, only while nothing is being drawn ----------------
        // Both of these block for tens of milliseconds (DuplicateOutput, a
        // resize), and doing that mid-fold is exactly what a fast opening makes
        // visible, so they wait for a moment when nothing is on screen.  The
        // gate fails closed, so a dead duplication with no usable snapshot has
        // already forced the effect off by here.
        if (!effectWanted) {
            // A duplication that came back DXGI_ERROR_ACCESS_LOST is dead and
            // can only be replaced.  Closing the lid is what kills it, so
            // retries are slow while the lid is shut and quick once it is open,
            // because the effect can only use a fresh capture from that point.
            const double recoveryInterval = lidClosedLatch ? 0.5 : 0.15;
            if (!capture.Healthy() &&
                sample.steadySeconds - lastRecoverySeconds >= recoveryInterval) {
                lastRecoverySeconds = sample.steadySeconds;
                if (capture.TryRestart(device.Device())) {
                    note("capture restarted", hingeAngle);
                }
            }

            // The replacement can report a different mode than the chain was
            // built for; follow it rather than drawing a mis-sized frame.
            if (capture.Healthy() && capture.Width() != 0 && capture.Height() != 0 &&
                (capture.Width() != contentWidth || capture.Height() != contentHeight)) {
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
                contentWidth = newWidth;
                contentHeight = newHeight;
                parameters.eyeDistancePx = static_cast<float>(eyeHeights * contentHeight);
                parameters.eyeUpPx =
                    static_cast<float>(options.eyeHeightHeights * contentHeight);
                parameters.windowWidth = static_cast<float>(width);
                parameters.windowHeight = static_cast<float>(height);
                parameters.originX = static_cast<float>(overlay.OriginX());
                parameters.originY = static_cast<float>(overlay.OriginY());
                safety.Reset();
            }
        }

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
            showSystemCursor(true);
            // Force the next frame on screen to be presented, whatever it is.
            lastPresentedDelta = 1.0e9;
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
                    contentDirty = true;
                    lastCaptureSeconds = SteadySeconds();
                    compositePointer();
                }
            }
        } else if (!lastEffectWanted) {
            // With the overlay excluded from capture there is nothing to hide
            // and nothing to wait for: the frame in hand is a real desktop and
            // the next one is a frame away.  Without that exclusion a capture
            // taken while the overlay is up would contain the overlay itself,
            // so it has to be hidden first and the frame taken with it down.
            if (!liveCapture) {
                if (overlay.IsShown()) {
                    overlay.Show(false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                hasContent = grabDesktop();
            }
            lastEffectWanted = true;
            note("effect on", hingeAngle);
        } else if (!hasContent) {
            // An earlier attempt came back empty; keep asking while the effect
            // wants to be visible.
            hasContent = grabDesktop();
        }

        // Nothing is drawn without a frame that was captured for this pass: a
        // stale texture is worse than no effect at all.
        if (effectWanted && hasContent) {
            overlay.Show(true);
            showSystemCursor(false);  // the pointer is part of the picture now
        }
        // Read the real window state rather than our own bookkeeping: if the
        // system refuses to show the window, the status line must say so.
        const bool overlayVisible = IsWindowVisible(overlay.Handle()) != FALSE;

        if (effectWanted && hasContent) {
            // Another topmost window can steal the front slot; re-assert it
            // periodically so the effect cannot end up hidden behind something.
            if ((++loopCount % 120) == 0) {
                overlay.BringToFront();
            }

            // ---- live content ---------------------------------------------
            // One non-blocking read per frame at most, and by default much less
            // often than that: every AcquireNextFrame keeps DWM on its capture
            // composition path, and other windows' acrylic backdrops cannot stay
            // cached there, which is what the flicker was.  The duplication only
            // hands back a frame when the desktop actually changed, and the mip
            // chain is only rebuilt when a frame arrived, so a static desktop
            // costs nothing either way.
            const bool captureDue =
                options.captureRateHz <= 0.0 ||
                (sample.steadySeconds - lastLiveCaptureSeconds) >=
                    (1.0 / options.captureRateHz);
            if (liveCapture && captureDue && capture.Healthy() &&
                capture.AcquireFrame(0)) {
                lastLiveCaptureSeconds = sample.steadySeconds;
                capture.CopyFrameTo(device.Context(), content.Get());
                capture.ReleaseFrame();
                ++captureCount;
                hasContent = true;
                contentDirty = true;
                lastCaptureSeconds = SteadySeconds();
                compositePointer();
            }

            // The blur samples the prefiltered chain, so it is rebuilt whenever
            // the content has been written since the last frame.
            bool contentUpdated = false;
            if (contentDirty) {
                renderer.UpdateContent(device.Context(), content.Get());
                contentDirty = false;
                contentUpdated = true;
            }

            // ---- present only what changed --------------------------------
            // Every Present re-invalidates the pane's area in the composition,
            // and that churn is what other windows' blur reacts to.  When
            // neither the picture (the angle) nor its content moved, there is
            // nothing to show that is not already on screen, so nothing is
            // presented at all: a lid held still then costs no composition work
            // whatsoever.
            const bool pictureMoved =
                std::abs(renderedDelta - lastPresentedDelta) > 0.02;
            if (!contentUpdated && !pictureMoved) {
                // Nothing to draw; the swap chain keeps showing the last frame.
            } else {
            lastPresentedDelta = renderedDelta;
            device.BeginFrame(0.0f, 0.0f, 0.0f, 1.0f);
            renderer.Render(device.Context(), content.Get(),
                            device.BackBuffer(),
                            static_cast<float>(renderedDelta), parameters);

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
        }

        // ---- status line --------------------------------------------------
        // Always available, but the console repaints on every write and an
        // acrylic terminal re-blurs its backdrop on every repaint, so --quiet
        // leaves it alone.
        if (!options.quiet && sample.steadySeconds - lastReportSeconds >= 0.25) {
            lastReportSeconds = sample.steadySeconds;
            std::string line =
                "\r" + FormatLine(preview ? "PREV " : (effectWanted ? "FOLD " : "idle "),
                                  hingeAngle, renderedDelta,
                                  anchorAngle,
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
    // Hand the display and system idle timeouts back to Windows.
    SetThreadExecutionState(ES_CONTINUOUS);
    showSystemCursor(true);
    cursor.Destroy();
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
