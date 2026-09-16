// ---------------------------------------------------------------------------
//  ConsoleUi.h
//
//  Terminal presentation only -- no sensor or estimation logic lives here.
//  Output is plain ASCII so the report survives any console code page.
// ---------------------------------------------------------------------------
#pragma once

#include "../animation/FoldAnimationController.h"
#include "../hinge/HingeTypes.h"
#include "../sensors/CustomSensorManager.h"
#include "../sensors/HidSensorEnumerator.h"
#include "../sensors/SensorManager.h"
#include "../sensors/SensorTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dragonfly::ui {

// ANSI escape handling with a redirect-safe fallback: when stdout is not a
// console, every control sequence becomes a no-op and the live view degrades to
// a periodic plain-text dump.
class Terminal {
public:
    Terminal();
    ~Terminal();

    bool Interactive() const { return m_interactive; }

    void EnterScreen();
    void LeaveScreen();
    void Home();
    void HideCursor(bool hide);
    void Write(const std::string& text);

private:
    bool m_interactive = false;
    bool m_inScreen = false;
};

std::string FormatStartupReport(const std::vector<ChannelInfo>& channels,
                                const std::vector<CustomSensorCandidate>& candidates,
                                const LidModeInfo& lidInfo,
                                const std::vector<HidDeviceInfo>& hidDevices,
                                const std::string& logPath,
                                const std::string& configPath,
                                uint32_t requestedIntervalMs,
                                bool hidRawRead);

// Everything the live frame needs, bundled so the signature stays readable.
struct LiveFrameInput {
    const SensorSample* sample = nullptr;
    const std::vector<ChannelInfo>* channels = nullptr;
    const LidModeInfo* lidInfo = nullptr;
    const HingeState* hinge = nullptr;
    const FoldAnimationState* animation = nullptr;

    int lidModeValue = -1;
    double lidModeAgeSeconds = -1.0;
    uint64_t rowsWritten = 0;
    double fps = 0.0;
    std::string logPath;
    std::string statusLine;
};

std::string FormatLiveFrame(const LiveFrameInput& input);

} // namespace dragonfly::ui
