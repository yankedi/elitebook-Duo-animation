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
    bool help = false;
};

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
        "The scene is driven by TILT: the angle between the screen normal and the\n"
        "world's up direction, taken from the world-vertical component of the screen\n"
        "normal.  It is absolute (0 = closed, 90 = upright, 180 = folded back) and it\n"
        "ignores yaw entirely, so rotating the whole machine on the desk does not move\n"
        "the panel at all.\n"
        "\n"
        "Tilting the whole machine (pitch / roll) still shifts it: a single IMU cannot\n"
        "tell the machine's own motion apart from lid motion.  That is a physical\n"
        "limit, not a bug.\n"
        "\n"
        "  STABLE   hinge model driven by tilt (default)\n"
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

        dragonfly::OrientationVisual visual;
        visual.alphaDeg = angles.alphaDeg;
        visual.betaDeg = angles.betaDeg;
        visual.gammaDeg = angles.gammaDeg;
        visual.rotation = tracker.Rotation();
        visual.relativeRotation = tracker.RelativeRotation();
        tracker.RelativeNormal(visual.relativeNormalX, visual.relativeNormalY,
                               visual.relativeNormalZ);
        tracker.Normal(visual.normalX, visual.normalY, visual.normalZ);
        visual.tiltDeg = tracker.TiltDegrees();
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

            char buffer[400];
            const int written = std::snprintf(
                buffer, sizeof(buffer),
                "%-7s  tilt %6.1f deg  | normal %6.3f %6.3f %6.3f  | "
                "a %6.1f  b %6.1f  g %7.1f%s%s%s",
                modeName(mode), visual.tiltDeg,
                visual.normalX, visual.normalY, visual.normalZ,
                angles.alphaDeg, angles.betaDeg, angles.gammaDeg,
                angles.gimbalLock ? "  GIMBAL" : "",
                angles.foldedBeta ? "  FOLDED" : "",
                visual.valid ? "" : "  (no orientation data)");

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
        return 0;
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
    if (config.debugWindowEnabled && !options.orientationDemo) {
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
    if (options.logging && !options.orientationDemo) {
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

    if (options.logging) {
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
        const int result = RunOrientationDemo(options, sensors, terminal);
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
