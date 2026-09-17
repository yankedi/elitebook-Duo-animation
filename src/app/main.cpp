// ---------------------------------------------------------------------------
//  main.cpp -- Dragonfly sensor diagnostic + fold estimator (Milestone 5A)
//
//  HP Elite Dragonfly G2, Windows 11 x64.
//
//  Pipeline:
//      SensorManager + CustomSensorManager       (acquisition, unchanged)
//          -> SensorSample
//          -> HingeEstimator                     (fold progress)
//          -> FoldAnimationController            (smoothing for the renderer)
//          -> Console UI + DebugFoldWindow       (visualisation)
//          -> SensorLogger                       (async CSV)
//
//  The renderer never touches the sensor layer; everything goes through
//  FoldAnimationState.
// ---------------------------------------------------------------------------
#include "ConsoleUi.h"
#include "DisplaySafetyGate.h"
#include "FoldEffectMode.h"

#include "../animation/FoldAnimationController.h"
#include "../config/ConfigLoader.h"
#include "../graphics/DebugFoldWindow.h"
#include "../graphics/OrientationDemoWindow.h"
#include "../hinge/HingeEstimator.h"
#include "../orientation/OrientationTracker.h"
#include "../sensors/CustomSensorManager.h"
#include "../sensors/HidSensorEnumerator.h"
#include "../sensors/SensorLogger.h"
#include "../sensors/SensorManager.h"
#include "../sensors/SensorTypes.h"

#include <winrt/base.h>

#include <windows.h>

#include <conio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace ui = dragonfly::ui;

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

struct Options {
    double seconds = 0.0;              // 0 -> run until quit
    double rateHz = 50.0;
    uint32_t reportIntervalMs = 16;
    std::string logDirectory = "logs";
    bool logging = true;
    bool enumerateOnly = false;
    bool hidRaw = false;
    bool debugWindow = true;
    bool startManual = false;
    bool writeConfig = false;
    bool selfTest = false;
    bool orientationDemo = false;
    bool foldEffect = false;
    bool dumpFrames = false;
    bool allowDisplayOff = false;
    bool keepSystemAwake = false;
    std::wstring glass = L"reference";
    double glassPreviewDegrees = 0.0;
    double blurOverride = -1.0;
    double dispersionOverride = -1.0;
    double sheenOverride = -1.0;
    double edgeOverride = -1.0;
    double parallaxOverride = -1.0;
    double eyeDistanceMm = 0.0;
    bool help = false;
};

// Parses the value of "--name=value"; false when the number is missing.
bool ParseNumberArgument(const std::wstring& argument, size_t prefixLength,
                         double& value) {
    const std::wstring text = argument.substr(prefixLength);
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(text.c_str(), &end);
    if (end == text.c_str()) {
        return false;
    }
    value = parsed;
    return true;
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

const wchar_t* kHelp =
    L"DragonflySensorDiag -- HP Elite Dragonfly G2 diagnostic + fold estimator\n"
    L"\n"
    L"Usage: DragonflySensorDiag.exe [options]\n"
    L"\n"
    L"  --seconds N      stop after N seconds (default: run until Q/Ctrl+C)\n"
    L"  --rate HZ        console + CSV snapshot rate (default 50)\n"
    L"  --report-ms MS   requested sensor report interval (default 16)\n"
    L"  --log DIR        CSV directory (default: logs next to the executable)\n"
    L"  --no-log         disable CSV logging\n"
    L"  --no-window      do not open the debug fold window\n"
    L"  --manual         start with the estimator in manual mode\n"
    L"  --enumerate      print the device/sensor report and exit\n"
    L"  --hid-raw        also read one raw HID input report per sensor device\n"
    L"  --write-config   write the default config.json next to the executable\n"
    L"  --selftest       run the estimator against a synthetic gyro sequence\n"
    L"  --orientation-demo  native counterpart of testyourdevices.com/gyroscope-test/\n"
    L"  --fold-effect    show the live desktop folding as the lid moves\n"
    L"  --allow-display-off  let the display sleep while --fold-effect runs\n"
    L"  --keep-system-awake  block Modern Standby while the lid is shut\n"
    L"  --glass=MODE     frost | clear (default) | plain -- the pane's material\n"
    L"  --glass-preview=DEG  hold the effect at a fixed angle to judge the look\n"
    L"  --blur=N --dispersion=N --sheen=N --edge=N   material overrides\n"
    L"  --eye=MM        eye distance from the pane (default 450). Smaller = the\n"
    L"                  picture stays put more strongly while the lid moves\n"
    L"  --parallax=N    0 = picture glued to the panel, 1 = anchored in the room\n"
    L"  --help           this text\n"
    L"\n"
    L"Runtime keys: R reset, A auto/manual, +/- nudge, C calibrate, Q quit\n";

bool ParseDouble(const std::wstring& text, double& value) {
    try {
        size_t consumed = 0;
        const double parsed = std::stod(text, &consumed);
        if (consumed != text.size()) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseUint(const std::wstring& text, uint32_t& value) {
    try {
        size_t consumed = 0;
        const unsigned long parsed = std::stoul(text, &consumed);
        if (consumed != text.size()) {
            return false;
        }
        value = static_cast<uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseArguments(int argc, wchar_t** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        auto next = [&](std::wstring& out) {
            if (index + 1 >= argc) {
                return false;
            }
            out = argv[++index];
            return true;
        };

        if (argument == L"--help" || argument == L"-h") {
            options.help = true;
        } else if (argument == L"--seconds") {
            std::wstring value;
            if (!next(value) || !ParseDouble(value, options.seconds)) {
                return false;
            }
        } else if (argument == L"--rate") {
            std::wstring value;
            if (!next(value) || !ParseDouble(value, options.rateHz) || options.rateHz <= 0.0) {
                return false;
            }
        } else if (argument == L"--report-ms") {
            std::wstring value;
            if (!next(value) || !ParseUint(value, options.reportIntervalMs)) {
                return false;
            }
        } else if (argument == L"--log") {
            std::wstring value;
            if (!next(value)) {
                return false;
            }
            options.logDirectory = ToUtf8(value);
        } else if (argument == L"--no-log") {
            options.logging = false;
        } else if (argument == L"--no-window") {
            options.debugWindow = false;
        } else if (argument == L"--manual") {
            options.startManual = true;
        } else if (argument == L"--enumerate") {
            options.enumerateOnly = true;
        } else if (argument == L"--hid-raw") {
            options.hidRaw = true;
        } else if (argument == L"--write-config") {
            options.writeConfig = true;
        } else if (argument == L"--selftest") {
            options.selfTest = true;
        } else if (argument == L"--orientation-demo") {
            options.orientationDemo = true;
        } else if (argument == L"--fold-effect") {
            options.foldEffect = true;
        } else if (argument == L"--dump-frames") {
            options.dumpFrames = true;
        } else if (argument == L"--allow-display-off") {
            options.allowDisplayOff = true;
        } else if (argument == L"--keep-system-awake") {
            options.keepSystemAwake = true;
        } else if (argument.rfind(L"--glass-preview=", 0) == 0) {
            const std::wstring value = argument.substr(16);
            wchar_t* end = nullptr;
            const double degrees = std::wcstod(value.c_str(), &end);
            if (end == value.c_str() || degrees <= 0.0 || degrees > 90.0) {
                return false;
            }
            options.glassPreviewDegrees = degrees;
        } else if (argument.rfind(L"--glass=", 0) == 0) {
            const std::wstring which = argument.substr(8);
            if (which == L"glass" || which == L"frost" || which == L"clear") {
                options.glass = L"glass";
            } else if (which == L"plain") {
                options.glass = L"plain";
            } else if (which == L"reference") {
                options.glass = L"reference";
            } else {
                return false;
            }
        } else if (argument.rfind(L"--blur=", 0) == 0) {
            if (!ParseNumberArgument(argument, 7, options.blurOverride)) {
                return false;
            }
        } else if (argument.rfind(L"--dispersion=", 0) == 0) {
            if (!ParseNumberArgument(argument, 13, options.dispersionOverride)) {
                return false;
            }
        } else if (argument.rfind(L"--sheen=", 0) == 0) {
            if (!ParseNumberArgument(argument, 8, options.sheenOverride)) {
                return false;
            }
        } else if (argument.rfind(L"--edge=", 0) == 0) {
            if (!ParseNumberArgument(argument, 7, options.edgeOverride)) {
                return false;
            }
        } else if (argument.rfind(L"--parallax=", 0) == 0) {
            if (!ParseNumberArgument(argument, 11, options.parallaxOverride)) {
                return false;
            }
        } else if (argument.rfind(L"--eye=", 0) == 0) {
            if (!ParseNumberArgument(argument, 6, options.eyeDistanceMm)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

std::filesystem::path ExecutableDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (file.is_open()) {
        file << text;
    }
}

// --------------------------------------------------------------------------
//  Debug controls
// --------------------------------------------------------------------------
enum class DebugAction {
    None,
    Reset,
    ToggleManual,
    NudgeUp,
    NudgeDown,
    Calibrate,
    Quit,
};

DebugAction DecodeKey(int key) {
    switch (key) {
    case 'r':
    case 'R':
        return DebugAction::Reset;
    case 'a':
    case 'A':
        return DebugAction::ToggleManual;
    case '+':
    case '=':
    case 0xBB:  // VK_OEM_PLUS
        return DebugAction::NudgeUp;
    case '-':
    case '_':
    case 0xBD:  // VK_OEM_MINUS
        return DebugAction::NudgeDown;
    case 'c':
    case 'C':
        return DebugAction::Calibrate;
    case 'q':
    case 'Q':
    case 27:    // ESC
        return DebugAction::Quit;
    default:
        return DebugAction::None;
    }
}

int PollConsoleKey() {
    if (!_kbhit()) {
        return 0;
    }
    const int key = _getch();
    // Arrow keys and function keys arrive as a two byte 0x00/0xE0 prefix.
    if (key == 0x00 || key == 0xE0) {
        _getch();
        return 0;
    }
    return key;
}

// --------------------------------------------------------------------------
//  Estimator self test
//
//  Feeds a synthetic gyro sequence through the real estimator + animation
//  chain.  It answers the two questions that cannot be checked without a
//  physical fold: does the progress start moving on the FIRST motion sample,
//  and does it hold its value afterwards.
// --------------------------------------------------------------------------
void RunEstimatorSelfTest() {
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

    dragonfly::HingeEstimator estimator;
    dragonfly::HingeEstimatorConfig config;
    estimator.Configure(config);

    dragonfly::FoldAnimationController animation;
    animation.Configure(0.05, 0.25);

    dragonfly::SensorSample sample;
    sample.gyro.valid = true;
    sample.accel.valid = true;
    sample.acceleration = {0.0, -0.85, -0.53};
    sample.orientationSensor.valid = true;
    sample.lidModeChannel.valid = true;
    sample.lidMode = 1;

    const double dt = 1.0 / 60.0;
    double t = 0.0;

    std::printf("\n=== estimator self test (synthetic gyro, Y axis carries the fold) ===\n");
    std::printf("%-22s %6s %10s %10s %-9s %-6s %s\n", "phase", "t", "progress",
                "velocity", "direction", "axis", "region");

    auto phase = [&](const char* name, double rateDegPerSec, int frames) {
        for (int i = 0; i < frames; ++i) {
            sample.steadySeconds = t;
            // Only the Y axis carries motion; X and Z see sensor noise.
            sample.angularVelocity.x = 0.0008;
            sample.angularVelocity.y = rateDegPerSec * kDegToRad;
            sample.angularVelocity.z = -0.0006;

            estimator.Update(sample, dt);
            const dragonfly::HingeState hinge = estimator.State();
            animation.Update(hinge, dt);
            const dragonfly::FoldAnimationState fold = animation.State();

            const bool show = i < 3 || (rateDegPerSec == 0.0 && i % 60 == 0) ||
                              i == frames - 1;
            if (show) {
                std::printf("%-22s %6.2f %10.4f %+10.3f %-9s %-6s %s\n", name, t,
                            static_cast<double>(fold.progress),
                            static_cast<double>(fold.velocity),
                            dragonfly::ToString(hinge.direction),
                            hinge.axisIndex < 0 ? "n/a" : (hinge.axisIndex == 0 ? "X"
                                                      : hinge.axisIndex == 1 ? "Y" : "Z"),
                            dragonfly::ToString(hinge.region));
            }
            t += dt;
        }
    };

    phase("rest (baseline)", 0.0, 120);
    phase("open 90 deg/s", 90.0, 30);      // 0.5 s -> 45 deg of hinge travel
    phase("hold", 0.0, 90);
    phase("close 60 deg/s", -60.0, 60);    // 1.0 s -> 60 deg back
    phase("hold", 0.0, 120);
    phase("open 20 deg/s (slow)", 20.0, 90);  // slow fold must still register
    phase("hold", 0.0, 120);

    const dragonfly::HingeState finalState = estimator.State();
    std::printf("\nfinal: progress %.4f  direction %s  axis %s  sign %+.0f "
                "(resolved %s)  confidence %.2f\n",
                static_cast<double>(finalState.foldProgress),
                dragonfly::ToString(finalState.direction),
                finalState.axisIndex < 0 ? "n/a" : (finalState.axisIndex == 1 ? "Y" : "?"),
                static_cast<double>(finalState.axisSign),
                finalState.axisSignConfident ? "yes" : "no",
                static_cast<double>(finalState.confidence));
    std::printf("expected: progress moves within the first frames of every motion "
                "phase, holds while 'hold', and does not drift.\n\n");
}

// ==========================================================================
//  Safety gate self test
//
//  The gate is pure logic, and it is the piece that decides whether a stale
//  overlay may stay on screen -- the failure mode being reproduced is "close
//  the lid, open it again, get a frozen blurred frame until the activation
//  angle".  So the transitions are checked here rather than only by closing a
//  real lid: lid shut, display off, capture lost, sensor gone quiet, and the
//  settling delay after each.
// ==========================================================================
int RunSafetyGateSelfTest() {
    dragonfly::DisplaySafetyGate gate;

    int failures = 0;
    int checks = 0;

    auto expect = [&](const char* what, bool ready, bool expectedReady) {
        ++checks;
        const bool ok = (ready == expectedReady);
        if (!ok) {
            ++failures;
        }
        std::printf("  %-58s %-8s %s\n", what, ready ? "ready" : "hidden",
                    ok ? "ok" : "MISMATCH");
    };

    auto expectState = [&](const char* what, dragonfly::DisplaySafetyGate::State actual,
                           dragonfly::DisplaySafetyGate::State expected) {
        ++checks;
        const bool ok = (actual == expected);
        if (!ok) {
            ++failures;
        }
        std::printf("  %-58s %-22s %s\n", what,
                    dragonfly::DisplaySafetyGate::Text(actual),
                    ok ? "ok" : "MISMATCH");
    };

    std::printf("\n=== display safety gate self test ===\n");
    std::printf("  %-58s %-8s %s\n", "step", "effect", "");

    // A healthy environment still has to settle before capture restarts.
    expect("healthy, first call", gate.Update(false, true, true, true, 1.0), false);
    expect("0.4 s of healthy", gate.Update(false, true, true, true, 1.4), false);
    expect("0.7 s of healthy", gate.Update(false, true, true, true, 1.7), true);

    // Shutting the lid fails immediately, with no settling period.
    expect("lid shut  (no settle allowed)", gate.Update(true, true, true, true, 2.0), false);
    expectState("state after a lid close", gate.Current(),
                dragonfly::DisplaySafetyGate::State::Closed);
    // ... and reopening has to settle from scratch, so the frame captured
    // before the close can never be shown again.
    expect("reopened, 0.1 s", gate.Update(false, true, true, true, 2.1), false);
    expect("reopened, 0.6 s", gate.Update(false, true, true, true, 2.7), true);

    expect("display off", gate.Update(false, false, true, true, 3.0), false);
    expectState("state with the monitor off", gate.Current(),
                dragonfly::DisplaySafetyGate::State::DisplayOff);
    expect("display back, settling", gate.Update(false, true, true, true, 3.2), false);

    expect("duplication lost", gate.Update(false, true, false, true, 4.0), false);
    expectState("state with a lost duplication", gate.Current(),
                dragonfly::DisplaySafetyGate::State::CaptureLost);
    expect("capture back, settling", gate.Update(false, true, true, true, 4.2), false);

    expect("sensor silent for 2 s", gate.Update(false, true, true, false, 5.0), false);
    expectState("state with a silent sensor", gate.Current(),
                dragonfly::DisplaySafetyGate::State::SensorStale);

    // A gate that flaps is as bad as one that never closes.
    gate.Reset();
    expect("flap: healthy 0.4 s", gate.Update(false, true, true, true, 10.0), false);
    expect("flap: lid shut again", gate.Update(true, true, true, true, 10.5), false);
    expect("flap: healthy 0.4 s", gate.Update(false, true, true, true, 11.0), false);
    expect("flap: settled 0.9 s", gate.Update(false, true, true, true, 11.6), true);

    std::printf("\ngate self test: %d checks, %d mismatches -> %s\n\n", checks, failures,
                failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}

} // namespace

// ==========================================================================
//  Orientation demo
//
//  Native counterpart of https://testyourdevices.com/gyroscope-test/ :
//  the model follows the fused Windows OrientationSensor through the same
//  alpha/beta/gamma conversion and the same 75 ms low-pass the site's CSS
//  transition provides.
// ==========================================================================
namespace {

int RunOrientationDemo(const Options& options, dragonfly::SensorManager& sensors,
                       dragonfly::CustomSensorManager& customSensors,
                       ui::Terminal& terminal) {
    dragonfly::OrientationTracker tracker;
    // The site's only smoothing is `transition-transform duration-75`, so
    // 75 ms is the equivalent time constant.
    tracker.Configure(0.075);

    dragonfly::OrientationDemoWindow window;
    bool windowOpen = false;
    if (options.debugWindow) {
        windowOpen = window.Create(L"Orientation Demo", 720, 540);
        if (!windowOpen) {
            terminal.Write("WARNING: could not create the demo window; "
                           "falling back to console output only.\n");
        }
    }

    terminal.Write(
        "\nOrientation demo - the model follows the fused Windows OrientationSensor.\n"
        "\n"
        "progress = hingeAngle / 180, covering ONLY the closed..flat span:\n"
        "     0.0 = closed, 0.5 = upright, 1.0 = flat\n"
        "Past flat (Lid Mode 3+) progress is pinned to 1.0 and the effect switches\n"
        "off -- there is no second IMU, so beyond 180 degrees the pose cannot be\n"
        "told apart from whole-device motion, and a renderer would simply restore\n"
        "the untouched desktop at that point.\n"
        "\n"
        "The angles come from the world-vertical component of the screen normal, so\n"
        "rotating the whole machine on the desk (yaw) does not move the panel at all.\n"
        "Tilting the whole machine (pitch / roll) still shifts it: that is a physical\n"
        "limit of a single IMU, not a bug.\n"
        "\n"
        "  STABLE   hinge model driven by foldProgress (default)\n"
        "  WEBSITE  the site's rotateX(-beta) rotateY(gamma) formula, for comparison\n"
        "  FULL     the complete attitude matrix, so yaw moves the model too\n"
        "\n"
        "Keys: [F] cycle mode   [T] transpose   [R] reset smoothing   [Q] quit\n\n");

    using VisualMode = dragonfly::OrientationVisualMode;
    VisualMode mode = VisualMode::Stable;   // stable mapping by default
    double lastReportSeconds = -1.0;
    double lastSampleSeconds = 0.0;
    std::string lastLineLength;

    auto nextTick = std::chrono::steady_clock::now();
    const std::chrono::duration<double> period(1.0 / 60.0);

    bool referenceCaptured = false;
    // Which side of the flat position the lid is on.  Lid Mode 2 is the
    // transition band, so only states 1 and 3+ may change this -- that keeps the
    // resolved angle from flickering while the sensor hovers around 180.
    bool pastFlat = false;

    auto modeName = [](VisualMode value) -> const char* {
        switch (value) {
        case VisualMode::Website: return "WEBSITE";
        case VisualMode::Full: return "FULL";
        default: return "STABLE";
        }
    };

    auto handleKey = [&](int key) {
        switch (key) {
        case 'f':
        case 'F':
            mode = (mode == VisualMode::Stable)   ? VisualMode::Website
                   : (mode == VisualMode::Website) ? VisualMode::Full
                                                   : VisualMode::Stable;
            break;
        case 't':
        case 'T':
            tracker.SetTranspose(!tracker.Transposed());
            break;
        case 'r':
        case 'R':
            tracker.Reset();
            referenceCaptured = false;  // capture the new pose as the reference
            break;
        case 'q':
        case 'Q':
        case 27:
            g_stop.store(true);
            break;
        default:
            break;
        }
    };

    while (!g_stop.load()) {
        const int consoleKey = PollConsoleKey();
        if (consoleKey != 0) {
            handleKey(consoleKey);
        }
        if (windowOpen) {
            if (!window.PumpMessages()) {
                windowOpen = false;
                terminal.Write("\nDemo window closed; console output continues.\n");
            } else if (const int windowKey = window.ConsumeKeyPress(); windowKey != 0) {
                handleKey(windowKey);
            }
        }

        const dragonfly::SensorSample sample = sensors.Snapshot();

        double dt = 1.0 / 60.0;
        if (lastSampleSeconds > 0.0 && sample.steadySeconds > lastSampleSeconds) {
            dt = sample.steadySeconds - lastSampleSeconds;
        }
        lastSampleSeconds = sample.steadySeconds;

        tracker.Update(sample, dt);

        // The scene frame is the reference attitude: the screen starts upright
        // facing the camera, and the fixed base only makes sense once that
        // frame is pinned.
        if (!referenceCaptured && tracker.Valid()) {
            tracker.CaptureReference();
            referenceCaptured = true;
            terminal.Write("\nReference attitude captured (only the FULL mode uses it;\n"
                           "STABLE is driven by the absolute tilt).\n");
        }

        const dragonfly::OrientationAngles angles = tracker.Angles();
        const int lidMode = customSensors.LidMode();

        // ---- resolve the full 0..360 hinge angle --------------------------
        // Tilt alone is |180 - theta|: it folds back at the flat position and
        // cannot tell 179 from 181 degrees.  Lid Mode says which side we are on.
        if (lidMode >= 3) {
            pastFlat = true;
        } else if (lidMode == 1) {
            pastFlat = false;
        }
        const double tiltDeg = tracker.TiltDegrees();
        const double hingeAngleDeg = pastFlat ? (180.0 + tiltDeg) : (180.0 - tiltDeg);

        // The visual only covers the closed..flat span.  Past flat the effect is
        // switched off outright: the progress is pinned to 1.0 so a renderer
        // would return to the untouched desktop instead of following the pose,
        // which a single IMU cannot resolve out there anyway.
        const double foldProgress =
            pastFlat ? 1.0 : std::clamp(hingeAngleDeg / 180.0, 0.0, 1.0);

        dragonfly::OrientationVisual visual;
        visual.alphaDeg = angles.alphaDeg;
        visual.betaDeg = angles.betaDeg;
        visual.gammaDeg = angles.gammaDeg;
        visual.rotation = tracker.Rotation();
        visual.relativeRotation = tracker.RelativeRotation();
        tracker.RelativeNormal(visual.relativeNormalX, visual.relativeNormalY,
                               visual.relativeNormalZ);
        tracker.Normal(visual.normalX, visual.normalY, visual.normalZ);
        visual.tiltDeg = tiltDeg;
        visual.hingeAngleDeg = hingeAngleDeg;
        visual.foldProgress = foldProgress;
        visual.lidMode = lidMode;
        visual.pastFlat = pastFlat;
        visual.mode = mode;
        visual.transposed = tracker.Transposed();
        visual.valid = tracker.Valid();
        visual.gimbalLock = angles.gimbalLock;
        visual.foldedBeta = angles.foldedBeta;

        if (windowOpen) {
            window.Render(visual);
            window.UpdateTitle(visual);
        }

        if (sample.steadySeconds - lastReportSeconds >= 0.25) {
            lastReportSeconds = sample.steadySeconds;

            char lidText[8] = {};
            if (lidMode < 0) {
                std::snprintf(lidText, sizeof(lidText), "--");
            } else {
                std::snprintf(lidText, sizeof(lidText), "%d", lidMode);
            }

            char buffer[460];
            const int written = std::snprintf(
                buffer, sizeof(buffer),
                "%-7s  hinge %6.1f   progress %5.3f%s   tilt %5.1f   lid %s  | "
                "normal %6.3f %6.3f %6.3f  | a %6.1f  b %6.1f  g %7.1f%s",
                modeName(mode), hingeAngleDeg, foldProgress,
                pastFlat ? " (effect off: past flat)" : "",
                tiltDeg, lidText,
                visual.normalX, visual.normalY, visual.normalZ,
                angles.alphaDeg, angles.betaDeg, angles.gammaDeg,
                angles.gimbalLock ? "  GIMBAL" : "");

            std::string line =
                "\r" + std::string(buffer, written < 0 ? 0 : static_cast<size_t>(written));
            if (line.size() < lastLineLength.size()) {
                line += std::string(lastLineLength.size() - line.size(), ' ');
            }
            lastLineLength = line;
            terminal.Write(line);
        }

        if (options.seconds > 0.0 && sample.steadySeconds >= options.seconds) {
            break;
        }

        nextTick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
        const auto now = std::chrono::steady_clock::now();
        if (nextTick < now) {
            nextTick = now;
        }
        std::this_thread::sleep_until(nextTick);
    }

    terminal.Write("\nOrientation demo stopped.\n");
    window.Destroy();
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Physical pixels everywhere: the overlay window and the desktop duplication
    // API both work in real screen coordinates rather than DPI-scaled ones.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Options options;
    if (!ParseArguments(argc, argv, options)) {
        std::fputs("Invalid arguments. Use --help.\n", stderr);
        return 1;
    }
    if (options.help) {
        const std::string utf8 = ToUtf8(std::wstring(kHelp));
        std::fwrite(utf8.data(), 1, utf8.size(), stdout);
        return 0;
    }

    if (options.selfTest) {
        RunEstimatorSelfTest();
        return RunSafetyGateSelfTest();
    }

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    ui::Terminal terminal;

    const std::filesystem::path executableDirectory = ExecutableDirectory();

    // ---- configuration ---------------------------------------------------
    const std::vector<std::filesystem::path> configRoots = {
        executableDirectory,
        executableDirectory.parent_path(),
        executableDirectory.parent_path().parent_path(),
        std::filesystem::current_path(),
    };
    dragonfly::AppConfig config = dragonfly::LoadAppConfig(configRoots);

    if (options.writeConfig) {
        const std::filesystem::path target = executableDirectory / "config" / "config.json";
        const bool written = dragonfly::WriteConfigFile(target, config);
        terminal.Write(std::string("Config ") + (written ? "written to " : "NOT written to ") +
                       target.string() + "\n");
        if (written) {
            return 0;
        }
    }

    if (!options.debugWindow) {
        config.debugWindowEnabled = false;
    }

    // ---- sensors ---------------------------------------------------------
    dragonfly::SensorManager sensors;
    sensors.Start(options.reportIntervalMs);

    dragonfly::CustomSensorManager customSensors;
    const std::vector<dragonfly::CustomSensorCandidate> candidates = customSensors.Enumerate();
    customSensors.Start(options.reportIntervalMs);

    // ---- device enumeration ----------------------------------------------
    std::vector<dragonfly::HidDeviceInfo> hidDevices =
        dragonfly::EnumerateHidDevices(options.hidRaw);
    for (dragonfly::HidDeviceInfo& device : dragonfly::EnumerateInterfacesAsHidDevices(
             dragonfly::kSensorInterfaceClassGuidText, options.hidRaw)) {
        hidDevices.push_back(std::move(device));
    }
    for (dragonfly::HidDeviceInfo& device : dragonfly::EnumerateInterfacesAsHidDevices(
             dragonfly::kLidModeInterfaceClassGuidText, options.hidRaw)) {
        hidDevices.push_back(std::move(device));
    }

    // ---- estimator + animation -------------------------------------------
    dragonfly::HingeEstimator estimator;
    estimator.Configure(config.hinge);

    dragonfly::FoldAnimationController animation;
    animation.Configure(config.animationSmoothingTauSeconds,
                        config.animationSnapThresholdProgress);

    // ---- debug window -----------------------------------------------------
    // The fold window belongs to the estimator pipeline only; the orientation
    // demo opens its own window, and creating both would leave an empty white
    // "Dragonfly Fold Debug" window sitting behind the demo.
    dragonfly::DebugFoldWindow debugWindow;
    bool windowOpen = false;
    if (config.debugWindowEnabled && !options.orientationDemo && !options.foldEffect) {
        windowOpen = debugWindow.Create(L"Dragonfly Fold Debug",
                                        config.debugWindowWidth, config.debugWindowHeight);
        if (!windowOpen) {
            terminal.Write("WARNING: debug fold window could not be created "
                           "(D3D11 unavailable?); continuing with console output only.\n");
        }
    }

    // ---- logging ---------------------------------------------------------
    // The demo is a visual check, not a measurement session: it does not open a
    // CSV, so running it does not litter the logs directory.
    dragonfly::SensorLogger logger;
    std::string logPath;
    std::filesystem::path logDirectory;
    if (options.logging && !options.orientationDemo && !options.foldEffect) {
        logDirectory = executableDirectory / options.logDirectory;
        std::error_code directoryError;
        std::filesystem::create_directories(logDirectory, directoryError);
        if (!options.enumerateOnly && logger.Open(logDirectory)) {
            logPath = logger.Path();
        }
    }

    // ---- startup report --------------------------------------------------
    const dragonfly::LidModeInfo lidInfo = customSensors.Info();
    const std::string report = ui::FormatStartupReport(
        sensors.Channels(), candidates, lidInfo, hidDevices, logPath, config.loadedFrom,
        options.reportIntervalMs, options.hidRaw);
    terminal.Write(report);

    if (options.logging && !logDirectory.empty() && !options.orientationDemo &&
        !options.foldEffect) {
        const std::filesystem::path reportPath = logDirectory / "startup-report.txt";
        WriteTextFile(reportPath, report);
        terminal.Write("Report written to: " + reportPath.string() + "\n");
    }

    if (options.enumerateOnly) {
        sensors.Stop();
        customSensors.Stop();
        logger.Close();
        debugWindow.Destroy();
        return 0;
    }

    if (options.orientationDemo) {
        const int result = RunOrientationDemo(options, sensors, customSensors, terminal);
        sensors.Stop();
        customSensors.Stop();
        logger.Close();
        debugWindow.Destroy();
        winrt::uninit_apartment();
        return result;
    }

    if (options.foldEffect) {
        dragonfly::FoldEffectOptions foldOptions;
        foldOptions.seconds = options.seconds;
        foldOptions.dumpFrames = options.dumpFrames;
        foldOptions.keepDisplayAwake = !options.allowDisplayOff;
        foldOptions.keepSystemAwake = options.keepSystemAwake;
        if (options.glass == L"glass") {
            foldOptions.glassPreset = dragonfly::FoldEffectOptions::GlassPreset::Glass;
        } else if (options.glass == L"plain") {
            foldOptions.glassPreset = dragonfly::FoldEffectOptions::GlassPreset::Plain;
        } else {
            foldOptions.glassPreset =
                dragonfly::FoldEffectOptions::GlassPreset::Reference;
        }
        foldOptions.previewDeltaDegrees = options.glassPreviewDegrees;
        foldOptions.blurOverride = static_cast<float>(options.blurOverride);
        foldOptions.dispersionOverride = static_cast<float>(options.dispersionOverride);
        foldOptions.sheenOverride = static_cast<float>(options.sheenOverride);
        foldOptions.edgeOverride = static_cast<float>(options.edgeOverride);
        foldOptions.parallaxOverride = static_cast<float>(options.parallaxOverride);
        foldOptions.eyeDistanceMm = static_cast<float>(options.eyeDistanceMm);
        const int result =
            dragonfly::RunFoldEffect(foldOptions, sensors, customSensors, g_stop, terminal);
        sensors.Stop();
        customSensors.Stop();
        logger.Close();
        debugWindow.Destroy();
        winrt::uninit_apartment();
        return result;
    }

    if (options.startManual) {
        estimator.SetManualEnabled(true);
    }

    terminal.Write("Starting live view (keys: R reset, A auto/manual, +/- nudge, "
                   "C calibrate, Q quit)...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // ---- live loop --------------------------------------------------------
    terminal.EnterScreen();
    terminal.HideCursor(true);

    const std::chrono::duration<double> period(1.0 / options.rateHz);
    const double drawInterval = 1.0 / 20.0;
    const double manualStep = config.hinge.manualStepPerPress;

    double nextDraw = 0.0;
    double lastSampleSeconds = 0.0;
    double fpsWindowStart = 0.0;
    uint64_t fpsFrames = 0;
    double fps = 0.0;
    double statusUntil = 0.0;
    std::string statusLine;
    auto nextTick = std::chrono::steady_clock::now();

    while (!g_stop.load()) {
        // ---- input --------------------------------------------------------
        const int consoleKey = PollConsoleKey();
        if (consoleKey != 0) {
            const DebugAction action = DecodeKey(consoleKey);
            switch (action) {
            case DebugAction::Reset:
                estimator.Reset();
                animation.Reset(estimator.State().foldProgress);
                statusLine = "estimator reset";
                statusUntil = dragonfly::SteadySeconds() + 2.0;
                break;
            case DebugAction::ToggleManual: {
                const bool enable = !estimator.ManualEnabled();
                if (enable) {
                    estimator.ManualSetProgress(animation.State().progress);
                }
                estimator.SetManualEnabled(enable);
                statusLine = enable ? "MANUAL mode (sensor integration paused)"
                                    : "AUTO mode (sensor driven)";
                statusUntil = dragonfly::SteadySeconds() + 2.5;
                break;
            }
            case DebugAction::NudgeUp:
                if (!estimator.ManualEnabled()) {
                    estimator.ManualSetProgress(animation.State().progress);
                    estimator.SetManualEnabled(true);
                }
                estimator.ManualNudge(manualStep);
                statusLine = "manual progress +" + std::to_string(manualStep);
                statusUntil = dragonfly::SteadySeconds() + 1.5;
                break;
            case DebugAction::NudgeDown:
                if (!estimator.ManualEnabled()) {
                    estimator.ManualSetProgress(animation.State().progress);
                    estimator.SetManualEnabled(true);
                }
                estimator.ManualNudge(-manualStep);
                statusLine = "manual progress -" + std::to_string(manualStep);
                statusUntil = dragonfly::SteadySeconds() + 1.5;
                break;
            case DebugAction::Calibrate:
                statusLine = "calibration is not part of v1; auto axis detection is active";
                statusUntil = dragonfly::SteadySeconds() + 3.0;
                break;
            case DebugAction::Quit:
                g_stop.store(true);
                break;
            case DebugAction::None:
                break;
            }
        }

        if (windowOpen) {
            if (!debugWindow.PumpMessages()) {
                windowOpen = false;
                terminal.Write("\nDebug window closed; console-only mode.\n");
            } else {
                const int windowKey = debugWindow.ConsumeKeyPress();
                if (windowKey != 0) {
                    const DebugAction action = DecodeKey(windowKey);
                    if (action == DebugAction::Quit) {
                        g_stop.store(true);
                    } else if (action == DebugAction::Reset) {
                        estimator.Reset();
                        animation.Reset(estimator.State().foldProgress);
                    } else if (action == DebugAction::ToggleManual) {
                        const bool enable = !estimator.ManualEnabled();
                        if (enable) {
                            estimator.ManualSetProgress(animation.State().progress);
                        }
                        estimator.SetManualEnabled(enable);
                    } else if (action == DebugAction::NudgeUp) {
                        if (!estimator.ManualEnabled()) {
                            estimator.ManualSetProgress(animation.State().progress);
                            estimator.SetManualEnabled(true);
                        }
                        estimator.ManualNudge(manualStep);
                    } else if (action == DebugAction::NudgeDown) {
                        if (!estimator.ManualEnabled()) {
                            estimator.ManualSetProgress(animation.State().progress);
                            estimator.SetManualEnabled(true);
                        }
                        estimator.ManualNudge(-manualStep);
                    }
                }
            }
        }

        // ---- acquire + estimate -------------------------------------------
        dragonfly::SensorSample sample = sensors.Snapshot();
        sample.lidMode = customSensors.LidMode();
        sample.lidModeChannel = customSensors.LidModeStamp();

        double dt = 1.0 / 60.0;
        if (lastSampleSeconds > 0.0 && sample.steadySeconds > lastSampleSeconds) {
            dt = sample.steadySeconds - lastSampleSeconds;
        }
        lastSampleSeconds = sample.steadySeconds;

        estimator.Update(sample, dt);
        const dragonfly::HingeState hinge = estimator.State();
        animation.Update(hinge, dt);
        const dragonfly::FoldAnimationState fold = animation.State();

        // ---- log ----------------------------------------------------------
        dragonfly::CsvRecord record;
        record.wallUnixMs = sample.wallUnixMs;
        record.steadySeconds = sample.steadySeconds;
        record.hasAccel = sample.accel.valid;
        record.accel = sample.acceleration;
        record.hasGyro = sample.gyro.valid;
        record.gyro = sample.angularVelocity;
        record.hasOrientation = sample.orientationSensor.valid;
        record.quat = sample.orientation;
        record.hasInclination = sample.inclinometer.valid;
        record.pitchDeg = sample.pitchDeg;
        record.rollDeg = sample.rollDeg;
        record.yawDeg = sample.yawDeg;
        record.hasCompass = sample.compass.valid;
        record.headingDeg = sample.headingDeg;
        record.hasHinge = sample.hingeAngle.valid;
        record.hingeAngleDeg = sample.hingeAngleDeg;
        record.lidMode = sample.lidMode;
        record.simpleOrientation = sample.simpleOrientationChannel.valid
                                       ? sample.simpleOrientation
                                       : -1;
        record.foldProgress = fold.progress;
        record.foldVelocity = fold.velocity;
        record.foldConfidence = fold.confidence;
        logger.Push(record);

        // ---- fps ----------------------------------------------------------
        ++fpsFrames;
        if (fpsWindowStart <= 0.0) {
            fpsWindowStart = sample.steadySeconds;
        } else if (sample.steadySeconds - fpsWindowStart >= 0.5) {
            fps = static_cast<double>(fpsFrames) /
                  (sample.steadySeconds - fpsWindowStart);
            fpsFrames = 0;
            fpsWindowStart = sample.steadySeconds;
        }

        // ---- present ------------------------------------------------------
        if (sample.steadySeconds >= nextDraw) {
            nextDraw = sample.steadySeconds + drawInterval;

            const dragonfly::LidModeInfo currentLidInfo = customSensors.Info();

            ui::LiveFrameInput frameInput;
            frameInput.sample = &sample;
            frameInput.channels = &sensors.Channels();
            frameInput.lidInfo = &currentLidInfo;
            frameInput.hinge = &hinge;
            frameInput.animation = &fold;
            frameInput.lidModeValue = sample.lidMode;
            frameInput.lidModeAgeSeconds =
                sample.lidModeChannel.AgeSeconds(sample.steadySeconds);
            frameInput.rowsWritten = logger.RowsWritten();
            frameInput.fps = fps;
            frameInput.logPath = logPath;
            if (sample.steadySeconds < statusUntil) {
                frameInput.statusLine = statusLine;
            }

            std::string frame = ui::FormatLiveFrame(frameInput);
            if (terminal.Interactive()) {
                frame += "\x1b[J";
                terminal.Home();
            }
            terminal.Write(frame);
        }

        if (windowOpen) {
            debugWindow.Render(fold.progress, fold.confidence, fold.manual);
            if (sample.steadySeconds >= nextDraw) {
                debugWindow.UpdateTitle(fold.progress, fold.confidence, fold.manual);
            }
        }

        if (options.seconds > 0.0 && sample.steadySeconds >= options.seconds) {
            break;
        }

        nextTick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
        const auto now = std::chrono::steady_clock::now();
        if (nextTick < now) {
            nextTick = now;
        }
        std::this_thread::sleep_until(nextTick);
    }

    terminal.HideCursor(false);
    terminal.LeaveScreen();

    // ---- shutdown ---------------------------------------------------------
    sensors.Stop();
    customSensors.Stop();
    debugWindow.Destroy();
    logger.Close();

    terminal.Write("\nStopped. CSV rows written: " + std::to_string(logger.RowsWritten()) + "\n");
    if (!logPath.empty()) {
        terminal.Write("Log file: " + logPath + "\n");
    }
    terminal.Write("foldProgress is a visual quantity; no physical angle was estimated.\n");

    winrt::uninit_apartment();
    return 0;
}
