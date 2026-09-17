// ---------------------------------------------------------------------------
//  ConsoleUi.cpp
// ---------------------------------------------------------------------------
#include "ConsoleUi.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>

namespace dragonfly::ui {

namespace {

constexpr int kWidth = 100;
constexpr int kProgressBarWidth = 40;

std::string Signed(double value, int decimals, int width) {
    if (!std::isfinite(value)) {
        return std::string(static_cast<size_t>(width), ' ');
    }
    std::ostringstream stream;
    stream << std::showpos << std::fixed << std::setprecision(decimals)
           << std::setw(width) << value;
    return stream.str();
}

std::string Plain(double value, int decimals) {
    if (!std::isfinite(value)) {
        return "n/a";
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

std::string Line(const std::string& text) {
    return text + "\n";
}

std::string Separator(char fill = '-') {
    return std::string(static_cast<size_t>(kWidth), fill);
}

std::string Pad(const std::string& text, size_t width) {
    if (text.size() >= width) {
        return text.substr(0, width);
    }
    return text + std::string(width - text.size(), ' ');
}

const ChannelInfo* Find(const std::vector<ChannelInfo>& channels, const char* key) {
    for (const ChannelInfo& channel : channels) {
        if (channel.key == key) {
            return &channel;
        }
    }
    return nullptr;
}

std::string IntervalText(uint32_t reportMs, uint32_t minMs) {
    if (reportMs == 0 && minMs == 0) {
        return "n/a";
    }
    return std::to_string(reportMs) + " ms / min " + std::to_string(minMs) + " ms";
}

std::string ChannelHeader(const std::vector<ChannelInfo>& channels, const char* key) {
    const ChannelInfo* info = Find(channels, key);
    if (!info) {
        return "  (channel missing)";
    }
    if (!info->present) {
        return "  UNAVAILABLE: " + (info->note.empty() ? std::string("not present") : info->note);
    }
    std::string text = "  [" + IntervalText(info->reportIntervalMs, info->minReportIntervalMs) + "]";
    if (!info->deviceName.empty()) {
        text += "  " + info->deviceName;
    }
    return text;
}

std::string AgeText(const ChannelStamp& stamp, double now) {
    const double age = stamp.AgeSeconds(now);
    if (age < 0.0) {
        return "no data yet";
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "age %6.3f s", age);
    return buffer;
}

std::string PropertyDumpLines(const std::vector<CustomPropertyDump>& properties,
                              const std::string& indent) {
    std::string out;
    for (const CustomPropertyDump& property : properties) {
        out += Line(indent + property.key + " = " + property.value +
                    "   (" + property.typeName + ")");
    }
    if (properties.empty()) {
        out += Line(indent + "(no properties reported)");
    }
    return out;
}

std::string ProgressBar(float progress) {
    const float clamped = std::clamp(progress, 0.0f, 1.0f);
    int filled = static_cast<int>(clamped * static_cast<float>(kProgressBarWidth) + 0.5f);
    filled = std::clamp(filled, 0, kProgressBarWidth);
    std::string bar(static_cast<size_t>(filled), '#');
    bar += std::string(static_cast<size_t>(kProgressBarWidth - filled), '-');
    return "[" + bar + "]";
}

std::string AxisName(int index) {
    switch (index) {
    case 0: return "X";
    case 1: return "Y";
    case 2: return "Z";
    default: return "n/a";
    }
}

} // namespace

// ==========================================================================
//  Terminal
// ==========================================================================
Terminal::Terminal() {
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (handle && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        m_interactive = true;
        SetConsoleOutputCP(CP_UTF8);
    }
}

Terminal::~Terminal() {
    HideCursor(false);
    if (m_inScreen) {
        LeaveScreen();
    }
}

void Terminal::EnterScreen() {
    if (!m_interactive || m_inScreen) {
        return;
    }
    Write("\x1b[?1049h\x1b[2J\x1b[H");
    m_inScreen = true;
}

void Terminal::LeaveScreen() {
    if (!m_inScreen) {
        return;
    }
    Write("\x1b[?1049l");
    m_inScreen = false;
}

void Terminal::Home() {
    if (m_interactive) {
        Write("\x1b[H");
    }
}

void Terminal::HideCursor(bool hide) {
    if (!m_interactive) {
        return;
    }
    Write(hide ? "\x1b[?25l" : "\x1b[?25h");
}

void Terminal::Write(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

// ==========================================================================
//  Startup report
// ==========================================================================
std::string FormatStartupReport(const std::vector<ChannelInfo>& channels,
                                const std::vector<CustomSensorCandidate>& candidates,
                                const LidModeInfo& lidInfo,
                                const std::vector<HidDeviceInfo>& hidDevices,
                                const std::string& logPath,
                                const std::string& configPath,
                                uint32_t requestedIntervalMs,
                                bool hidRawRead) {
    std::string out;
    out += Line(Separator('='));
    out += Line("  HP Elite Dragonfly G2 - Dragonfly Sensor Diagnostic + Fold Estimator (v1)");
    out += Line("  Requested report interval: " + std::to_string(requestedIntervalMs) + " ms");
    out += Line("  CSV log : " + (logPath.empty() ? std::string("(disabled)") : logPath));
    out += Line("  Config  : " + (configPath.empty()
                                     ? std::string("(built-in defaults; no config.json found)")
                                     : configPath));
    out += Line("  NOTE    : foldProgress is a VISUAL progress (0..1), not a measured angle.");
    out += Line(Separator('='));

    out += Line("");
    out += Line("  [1] STANDARD Windows.Devices.Sensors CHANNELS");
    out += Line(Separator('-'));
    for (const ChannelInfo& channel : channels) {
        std::string state = channel.present ? "PRESENT" : "ABSENT ";
        out += Line("  " + Pad(channel.label, 26) + " " + state +
                    "  event=" + (channel.eventDriven ? "yes" : "no "));
        if (!channel.deviceId.empty()) {
            out += Line("      device id    : " + channel.deviceId);
        }
        if (!channel.deviceName.empty()) {
            out += Line("      name         : " + channel.deviceName);
        }
        if (!channel.manufacturer.empty() || !channel.model.empty()) {
            out += Line("      manufacturer : " + channel.manufacturer +
                        "   model: " + channel.model);
        }
        if (channel.present) {
            out += Line("      interval     : " +
                        IntervalText(channel.reportIntervalMs, channel.minReportIntervalMs));
        }
        if (!channel.note.empty()) {
            out += Line("      note         : " + channel.note);
        }
    }

    out += Line("");
    out += Line("  [2] CUSTOM SENSOR (Lid Mode) CANDIDATES");
    out += Line(Separator('-'));
    if (candidates.empty()) {
        out += Line("  No custom-sensor device matched the recorded sensor-type GUID");
        out += Line("  {00000300-766d-4333-8262-27e82dd158b1}, and no sensor-class device");
        out += Line("  advertised a lid-mode name.  See the raw HID dump below.");
    } else {
        for (const CustomSensorCandidate& candidate : candidates) {
            out += Line("  " + Pad(candidate.name.empty() ? "(no name)" : candidate.name, 44) +
                        (candidate.lidCandidate ? "[lid] " : "      ") +
                        (candidate.fromSensorTypeSelector ? "[guid]" : "      "));
            out += Line("      id           : " + candidate.id);
            if (!candidate.manufacturer.empty() || !candidate.model.empty()) {
                out += Line("      manufacturer : " + candidate.manufacturer +
                            "   model: " + candidate.model);
            }
            out += Line("      openable     : " + std::string(candidate.openable ? "yes" : "no"));
            if (!candidate.error.empty()) {
                out += Line("      error        : " + candidate.error);
            }
            if (candidate.openable && !candidate.firstReading.empty()) {
                out += Line("      first reading:");
                out += PropertyDumpLines(candidate.firstReading, "        ");
            }
        }
    }

    out += Line("");
    out += Line("  [3] LID MODE RESULT");
    out += Line(Separator('-'));
    if (lidInfo.present) {
        out += Line("  Sensor opened      : yes" +
                    std::string(lidInfo.usedFallbackPath ? " (fallback path)" : ""));
        out += Line("  device id          : " + lidInfo.deviceId);
        out += Line("  name               : " + lidInfo.deviceName);
        out += Line("  interval           : " +
                    IntervalText(lidInfo.reportIntervalMs, lidInfo.minReportIntervalMs));
        out += Line("  max batch size     : " + std::to_string(lidInfo.maxBatchSize));
        out += Line("  state property     : " +
                    (lidInfo.valueKey.empty() ? std::string("(not identified yet)")
                                              : lidInfo.valueKey));
        out += Line("  current value      : " +
                    (lidInfo.rawValue >= 0 ? std::to_string(lidInfo.rawValue)
                                           : std::string("no reading yet")));
        if (!lidInfo.note.empty()) {
            out += Line("  note               : " + lidInfo.note);
        }
        if (!lidInfo.lastProperties.empty()) {
            out += Line("  last reading properties:");
            out += PropertyDumpLines(lidInfo.lastProperties, "    ");
        }
    } else {
        out += Line("  Intel Lid Mode Sensor NOT AVAILABLE through CustomSensor.");
        if (!lidInfo.note.empty()) {
            out += Line("  reason             : " + lidInfo.note);
        }
    }

    out += Line("");
    out += Line("  [4] RAW HID DEVICE TREE (SetupAPI, independent of WinRT)");
    out += Line(Separator('-'));
    if (hidDevices.empty()) {
        out += Line("  No HID device interfaces found (unexpected).");
    } else {
        size_t sensorCount = 0;
        for (const HidDeviceInfo& device : hidDevices) {
            if (device.isSensorCollection) {
                ++sensorCount;
            }
        }
        out += Line("  HID interfaces present : " + std::to_string(hidDevices.size()) +
                    "   of which usage page 0x20 (Sensor): " + std::to_string(sensorCount));
        out += Line("");

        // Group by interface class: the Intel hub registers its collections
        // under private GUIDs instead of GUID_DEVINTERFACE_HID.
        std::vector<std::string> groups;
        for (const HidDeviceInfo& device : hidDevices) {
            if (std::find(groups.begin(), groups.end(), device.sourceGroup) == groups.end()) {
                groups.push_back(device.sourceGroup);
            }
        }

        for (const std::string& group : groups) {
            size_t count = 0;
            for (const HidDeviceInfo& device : hidDevices) {
                if (device.sourceGroup == group) {
                    ++count;
                }
            }
            out += Line("  Interface class: " + group + "   (" + std::to_string(count) +
                        " interfaces)");
            for (const HidDeviceInfo& device : hidDevices) {
                if (device.sourceGroup != group) {
                    continue;
                }
                char header[256];
                std::snprintf(header, sizeof(header),
                              "    VID_%04X PID_%04X  page 0x%04X usage 0x%04X  in %3u B%s",
                              device.vendorId, device.productId, device.usagePage, device.usage,
                              device.inputReportByteLength,
                              device.isSensorCollection ? "  [sensor-page]" : "");
                out += Line(std::string(header) + "  " +
                            (device.product.empty() ? "(no product string)" : device.product));
                out += Line("        instance    : " + device.instanceId);
                if (!device.sensorTypeGuid.empty()) {
                    out += Line("        sensor type : " + device.sensorTypeGuid);
                }
                if (!device.error.empty()) {
                    out += Line("        note        : " + device.error);
                }
                if (!device.rawReport.empty()) {
                    out += Line("        raw report  : " + device.rawReport);
                }
                for (const std::string& parsed : device.parsedUsages) {
                    out += Line("        decoded     : " + parsed);
                }
            }
            out += Line("");
        }
    }

    if (!hidRawRead) {
        out += Line("  (run with --hid-raw to also read one raw input report per sensor device)");
        out += Line("");
    }

    out += Line("  [5] COMMANDS");
    out += Line(Separator('-'));
    out += Line("  --seconds N     stop after N seconds (default: run until Ctrl+C)");
    out += Line("  --rate HZ       snapshot rate for console + CSV (default 50)");
    out += Line("  --report-ms MS  requested sensor report interval (default 16)");
    out += Line("  --log DIR       CSV directory (default: logs next to the executable)");
    out += Line("  --no-log        disable CSV logging");
    out += Line("  --enumerate     print this report and exit");
    out += Line("  --hid-raw       also read one raw input report per HID sensor");
    out += Line("  --no-window     do not open the debug fold window");
    out += Line("  --manual        start with the estimator in manual mode");
    out += Line("  --write-config  write the default config.json next to the executable");
    out += Line("  --selftest      run the estimator and safety-gate self tests");
    out += Line("  --orientation-demo  native counterpart of testyourdevices.com/gyroscope-test/");
    out += Line("  --fold-effect   show the live desktop folding as the lid moves");
    out += Line("  --dump-frames   save the captured desktop and rendered frames as BMP");
    out += Line("  --allow-display-off  let the display sleep while --fold-effect runs");
    out += Line("  --keep-system-awake  block Modern Standby while the lid is shut");
    out += Line("  --glass=MODE    frost | clear (default) | plain -- the pane's material");
    out += Line("  --glass-preview=DEG  hold the effect at a fixed angle to judge the look");
    out += Line("  --blur=N --dispersion=N --sheen=N --edge=N   material overrides");
    out += Line("  --eye=MM        eye distance from the pane (default 450 mm)");
    out += Line("  --parallax=N    0 = picture glued to the panel, 1 = anchored in the room");
    out += Line("  --no-layered-overlay  drop WS_EX_LAYERED (breaks click-through; for testing)");
    out += Line("  --no-capture-exclusion  do not exclude the pane from capture (snapshot mode)");
    out += Line("  --quiet         stop drawing the status line (acrylic terminals re-blur on repaint)");
    out += Line("  --fps=N         render rate while the effect is up (default 60)");
    out += Line("");
    out += Line("  Fold progress is a visual quantity; no physical angle is estimated.");
    out += Line("");

    return out;
}

// ==========================================================================
//  Live frame
// ==========================================================================
std::string FormatLiveFrame(const LiveFrameInput& input) {
    std::string out;

    const SensorSample& sample = *input.sample;
    const std::vector<ChannelInfo>& channels = *input.channels;
    const HingeState& hinge = *input.hinge;
    const FoldAnimationState& animation = *input.animation;

    out += Line(Separator('='));
    out += Line("  t = " + Plain(sample.steadySeconds, 2) + " s   rows = " +
                std::to_string(input.rowsWritten) + "   fps = " + Plain(input.fps, 1) +
                "   log = " + (input.logPath.empty() ? std::string("(disabled)") : input.logPath));
    out += Line(Separator('='));

    // ---- fold estimator ---------------------------------------------------
    out += Line("  FOLD ESTIMATOR" +
                std::string(animation.manual ? "   [MANUAL]" : "   [AUTO]"));
    out += Line("    progress   " + ProgressBar(animation.progress) + "  " +
                Plain(animation.progress, 3));
    out += Line("    direction  " + Pad(ToString(hinge.direction), 10) +
                "  region " + Pad(ToString(hinge.region), 12) +
                "  confidence " + Plain(animation.confidence, 2));
    out += Line("    velocity   " + Signed(animation.velocity, 3, 8) + " /s" +
                "        lid mode " + Pad(ToString(static_cast<RawLidMode>(hinge.rawLidMode)), 8) +
                " (raw " + std::to_string(hinge.rawLidMode) + ")");
    out += Line("    hinge axis " + Pad(AxisName(hinge.axisIndex), 3) +
                " sign " + Signed(hinge.axisSign, 0, 2) +
                "   rate " + Signed(hinge.axisRateDegPerSec, 1, 7) + " deg/s" +
                "   share " + Plain(hinge.axisShare, 2));
    out += Line("    sign resolved " + std::string(hinge.axisSignConfident ? "yes" : "no ") +
                "   whole-device motion " + std::string(hinge.wholeDeviceMotion ? "YES" : "no") +
                "   sensors " + std::string(hinge.sensorsValid ? "ok" : "--"));
    if (!input.statusLine.empty()) {
        out += Line("    status     " + input.statusLine);
    }
    out += Line(Separator('-'));

    // ---- raw sensors ------------------------------------------------------
    out += Line("  ACCELEROMETER (g)" + ChannelHeader(channels, "accelerometer"));
    out += Line("    X " + Signed(sample.acceleration.x, 4, 9) +
                "   Y " + Signed(sample.acceleration.y, 4, 9) +
                "   Z " + Signed(sample.acceleration.z, 4, 9) +
                "      " + AgeText(sample.accel, sample.steadySeconds));

    out += Line("  GYROMETER (rad/s)" + ChannelHeader(channels, "gyrometer"));
    out += Line("    X " + Signed(sample.angularVelocity.x, 5, 10) +
                "   Y " + Signed(sample.angularVelocity.y, 5, 10) +
                "   Z " + Signed(sample.angularVelocity.z, 5, 10) +
                "      " + AgeText(sample.gyro, sample.steadySeconds));

    out += Line("  ORIENTATION SENSOR" + ChannelHeader(channels, "orientation"));
    out += Line("    quat   X " + Signed(sample.orientation.x, 5, 9) +
                "   Y " + Signed(sample.orientation.y, 5, 9) +
                "   Z " + Signed(sample.orientation.z, 5, 9) +
                "   W " + Signed(sample.orientation.w, 5, 9));
    out += Line("    matrix [" + Signed(sample.rotationMatrix.m[0], 4, 8) + " " +
                Signed(sample.rotationMatrix.m[1], 4, 8) + " " +
                Signed(sample.rotationMatrix.m[2], 4, 8) + " ]");
    out += Line("           [" + Signed(sample.rotationMatrix.m[3], 4, 8) + " " +
                Signed(sample.rotationMatrix.m[4], 4, 8) + " " +
                Signed(sample.rotationMatrix.m[5], 4, 8) + " ]   " +
                AgeText(sample.orientationSensor, sample.steadySeconds));
    out += Line("           [" + Signed(sample.rotationMatrix.m[6], 4, 8) + " " +
                Signed(sample.rotationMatrix.m[7], 4, 8) + " " +
                Signed(sample.rotationMatrix.m[8], 4, 8) + " ]");

    out += Line("  INCLINOMETER" + ChannelHeader(channels, "inclinometer"));
    out += Line("    pitch " + Signed(sample.pitchDeg, 3, 9) +
                "   roll " + Signed(sample.rollDeg, 3, 9) +
                "   yaw " + Signed(sample.yawDeg, 3, 9) +
                "      " + AgeText(sample.inclinometer, sample.steadySeconds));

    out += Line("  COMPASS" + ChannelHeader(channels, "compass"));
    out += Line("    heading " + Signed(sample.headingDeg, 3, 9) +
                "      " + AgeText(sample.compass, sample.steadySeconds));

    out += Line("  SIMPLE ORIENTATION" + ChannelHeader(channels, "simple_orientation"));
    out += Line("    code " + std::to_string(sample.simpleOrientation) + " (" +
                SimpleOrientationLabel(sample.simpleOrientation) + ")");

    out += Line("  HINGE ANGLE SENSOR" + ChannelHeader(channels, "hinge_angle"));
    if (const ChannelInfo* hingeChannel = Find(channels, "hinge_angle");
        hingeChannel && hingeChannel->present) {
        out += Line("    angle " + Signed(sample.hingeAngleDeg, 3, 9) + " deg");
    } else {
        out += Line("    angle UNAVAILABLE (HingeAngleSensor.GetDefaultAsync() == null)");
    }

    out += Line("  INTEL LID MODE (CustomSensor, raw value only, on-change sensor)");
    if (input.lidModeValue >= 0) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "    raw = %d    age %.3f s    key %s",
                      input.lidModeValue, input.lidModeAgeSeconds,
                      input.lidInfo->valueKey.empty() ? "(unknown)"
                                                      : input.lidInfo->valueKey.c_str());
        out += Line(buffer);
    } else if (input.lidInfo->present) {
        out += Line("    sensor open, no value received yet");
    } else {
        out += Line("    UNAVAILABLE");
    }

    out += Line(Separator('-'));
    out += Line("  Keys: [R]eset  [A]auto/manual  [+]/[-] nudge progress  "
                "[C]alibrate  [Q]uit");
    out += Line("  Physical angle: not estimated in v1 (visual progress only)");
    out += Line(Separator('='));

    return out;
}

} // namespace dragonfly::ui
