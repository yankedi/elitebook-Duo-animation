// ---------------------------------------------------------------------------
//  CustomSensorManager.h
//
//  The Intel "Lid Mode Sensor" exposed by the Intel Sensor Hub is not a
//  standard Windows sensor: it is reached through
//  Windows.Devices.Sensors.Custom.CustomSensor, addressed either by its
//  sensor-type interface GUID or by device interface path.
//
//  Two independent opening paths are attempted, in this order:
//
//    1. CustomSensor::GetDeviceSelector(<sensor type guid>) ->
//       DeviceInformation::FindAllAsync -> CustomSensor::FromIdAsync
//    2. DeviceInformation::FindAllAsync(DeviceClass::Sensor), then FromIdAsync
//       on every device that looks like a lid/state sensor
//
//  The GUID used in path 1 comes from the project's own hardware research
//  notes (HP_Elite_Dragonfly_G2_Hinge_Sensor_Research.md, section 7) and is
//  treated as a *candidate*: it is verified against the machine at runtime and
//  the enumeration dump shows what was actually found.
//
//  The value reported by the sensor is a raw UInt32 state code.  This class
//  never maps it to degrees -- phase 1 only records what the hardware says.
// ---------------------------------------------------------------------------
#pragma once

#include "SensorTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dragonfly {

// One custom-sensor property as reported by the driver: "{guid} = value (type)".
struct CustomPropertyDump {
    std::string key;        // guid string
    std::string value;      // best-effort textual value
    std::string typeName;   // Windows::Foundation::PropertyType name, or runtime class name
};

struct CustomSensorCandidate {
    std::string id;          // device interface path
    std::string name;        // DeviceInformation.Name
    std::string manufacturer;
    std::string model;
    bool lidCandidate = false;          // name matched "lid"
    bool fromSensorTypeSelector = false;// matched the recorded sensor-type GUID
    bool openable = false;   // CustomSensor::FromIdAsync succeeded
    std::string error;       // why it could not be opened, if it could not
    std::vector<CustomPropertyDump> firstReading;  // properties of one reading, if opened
};

struct LidModeInfo {
    bool present = false;        // a candidate sensor was opened
    bool reading = false;        // at least one reading arrived / was polled
    bool usedFallbackPath = false;

    std::string deviceId;
    std::string deviceName;
    std::string manufacturer;
    std::string model;

    uint32_t minReportIntervalMs = 0;
    uint32_t reportIntervalMs = 0;
    uint32_t maxBatchSize = 0;

    int rawValue = -1;           // last raw state code, -1 = none
    std::string valueKey;        // which property was identified as the state code
    std::string note;

    std::vector<CustomPropertyDump> lastProperties;  // full dump of the last reading
};

class CustomSensorManager {
public:
    CustomSensorManager();
    ~CustomSensorManager();

    CustomSensorManager(const CustomSensorManager&) = delete;
    CustomSensorManager& operator=(const CustomSensorManager&) = delete;

    // Enumerates lid-mode candidates without subscribing to anything.
    // Safe to call before Start(); also safe when the sensor is absent.
    std::vector<CustomSensorCandidate> Enumerate();

    bool Start(uint32_t desiredReportIntervalMs);
    void Stop();

    int LidMode() const;                        // raw state code, -1 = unavailable
    ChannelStamp LidModeStamp() const;
    LidModeInfo Info() const;                   // snapshot copy

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dragonfly
