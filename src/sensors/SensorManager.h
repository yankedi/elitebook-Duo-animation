// ---------------------------------------------------------------------------
//  SensorManager.h
//
//  Wraps the standard Windows.Devices.Sensors surface:
//
//      Accelerometer, Gyrometer, OrientationSensor, Inclinometer, Compass,
//      SimpleOrientationSensor, HingeAngleSensor
//
//  Everything WinRT stays inside the .cpp (pimpl) so the rest of the project
//  never has to care about projection headers or header ordering rules.
//
//  The manager never invents data: a channel that is absent stays absent, and
//  a channel whose driver stops delivering stops advancing its timestamp.
// ---------------------------------------------------------------------------
#pragma once

#include "SensorTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dragonfly {

// Human readable name for Windows.Devices.Sensors.SimpleOrientation.
std::string SimpleOrientationLabel(int value);

struct ChannelInfo {
    std::string key;                    // stable identifier, e.g. "accelerometer"
    std::string label;                  // human readable, e.g. "Accelerometer"

    bool present = false;               // the sensor object exists on this machine
    bool eventDriven = false;           // ReadingChanged subscription succeeded
    bool currentReadingSupported = true;// GetCurrentReading() usable

    std::string deviceId;               // WinRT device interface path
    std::string deviceName;             // DeviceInformation.Name
    std::string manufacturer;
    std::string model;

    uint32_t minReportIntervalMs = 0;
    uint32_t reportIntervalMs = 0;

    std::string note;                   // "unavailable", error text, ...
};

class SensorManager {
public:
    SensorManager();
    ~SensorManager();

    SensorManager(const SensorManager&) = delete;
    SensorManager& operator=(const SensorManager&) = delete;

    // Opens every standard sensor and subscribes to its readings.
    // Returns true when at least one channel is live.
    bool Start(uint32_t desiredReportIntervalMs);

    // Unsubscribes, closes and releases every sensor object.
    void Stop();

    // Locks each channel briefly and copies the freshest reading of each.
    SensorSample Snapshot() const;

    const std::vector<ChannelInfo>& Channels() const noexcept { return m_channels; }

    const ChannelInfo* FindChannel(const std::string& key) const;

private:



    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::vector<ChannelInfo> m_channels;
};

} // namespace dragonfly
