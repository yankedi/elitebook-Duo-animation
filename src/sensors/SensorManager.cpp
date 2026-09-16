// ---------------------------------------------------------------------------
//  SensorManager.cpp
//
//  Standard Windows.Devices.Sensors acquisition.
//
//  Threading: readings arrive on WinRT thread-pool threads, the UI thread only
//  ever touches the small per-channel mutexes in Snapshot().  Critical sections
//  are a few loads/stores wide, so a 50 Hz consumer cannot disturb delivery.
// ---------------------------------------------------------------------------
#include "SensorManager.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Sensors.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace dragonfly {

namespace {

using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Sensors::Accelerometer;
using winrt::Windows::Devices::Sensors::AccelerometerReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::AccelerometerReadingType;
using winrt::Windows::Devices::Sensors::Compass;
using winrt::Windows::Devices::Sensors::CompassReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::Gyrometer;
using winrt::Windows::Devices::Sensors::GyrometerReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::HingeAngleSensor;
using winrt::Windows::Devices::Sensors::HingeAngleSensorReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::Inclinometer;
using winrt::Windows::Devices::Sensors::InclinometerReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::OrientationSensor;
using winrt::Windows::Devices::Sensors::OrientationSensorReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::SensorReadingType;
using winrt::Windows::Devices::Sensors::SimpleOrientation;
using winrt::Windows::Devices::Sensors::SimpleOrientationSensor;
using winrt::Windows::Devices::Sensors::SimpleOrientationSensorOrientationChangedEventArgs;

// --------------------------------------------------------------------------
//  Generic single-value channel: the only shared mutable state between the
//  WinRT callback thread and the consumer.
// --------------------------------------------------------------------------
template <typename T>
struct ChannelRead {
    T value{};
    bool valid = false;
    double steadySeconds = 0.0;
};

template <typename T>
struct Channel {
    mutable std::mutex mutex;
    bool valid = false;
    double steadySeconds = 0.0;
    T value{};

    void Store(T const& next) {
        std::lock_guard<std::mutex> lock(mutex);
        value = next;
        steadySeconds = SteadySeconds();
        valid = true;
    }

    ChannelRead<T> Read() const {
        std::lock_guard<std::mutex> lock(mutex);
        ChannelRead<T> out;
        out.value = value;
        out.valid = valid;
        out.steadySeconds = steadySeconds;
        return out;
    }
};

template <typename T>
void ApplyStamp(ChannelStamp& stamp, ChannelRead<T> const& read) {
    stamp.valid = read.valid;
    stamp.steadySeconds = read.steadySeconds;
}

struct OrientationValue {
    Quat quaternion;
    Mat3 matrix;
};

struct InclinationValue {
    double pitchDeg = 0.0;
    double rollDeg = 0.0;
    double yawDeg = 0.0;
};

// --------------------------------------------------------------------------
//  Property helpers
// --------------------------------------------------------------------------
std::string PropertyToString(winrt::Windows::Foundation::IInspectable const& value) {
    if (!value) {
        return {};
    }
    if (auto propertyValue = value.try_as<winrt::Windows::Foundation::IPropertyValue>()) {
        switch (propertyValue.Type()) {
        case winrt::Windows::Foundation::PropertyType::String:
            return winrt::to_string(propertyValue.GetString());
        case winrt::Windows::Foundation::PropertyType::UInt32:
            return std::to_string(propertyValue.GetUInt32());
        case winrt::Windows::Foundation::PropertyType::Int32:
            return std::to_string(propertyValue.GetInt32());
        case winrt::Windows::Foundation::PropertyType::UInt64:
            return std::to_string(propertyValue.GetUInt64());
        case winrt::Windows::Foundation::PropertyType::Boolean:
            return propertyValue.GetBoolean() ? "true" : "false";
        case winrt::Windows::Foundation::PropertyType::Guid:
            return winrt::to_string(winrt::to_hstring(propertyValue.GetGuid()));
        default:
            break;
        }
    }
    return {};
}

std::string DeviceProperty(DeviceInformation const& device, wchar_t const* key) {
    try {
        auto properties = device.Properties();
        if (properties && properties.HasKey(key)) {
            return PropertyToString(properties.Lookup(key));
        }
    } catch (winrt::hresult_error const&) {
    }
    return {};
}

struct DeviceMeta {
    std::string id;
    std::string name;
    std::string manufacturer;
    std::string model;
};

std::vector<DeviceMeta> QueryDevices(winrt::hstring const& selector) {
    std::vector<DeviceMeta> result;
    try {
        auto devices = DeviceInformation::FindAllAsync(selector).get();
        for (auto const& device : devices) {
            DeviceMeta meta;
            meta.id = winrt::to_string(device.Id());
            meta.name = winrt::to_string(device.Name());
            meta.manufacturer = DeviceProperty(device, L"System.Devices.Manufacturer");
            meta.model = DeviceProperty(device, L"System.Devices.ModelName");
            result.push_back(std::move(meta));
        }
    } catch (winrt::hresult_error const&) {
    }
    return result;
}

// Fills the naming half of a channel record from a DeviceInformation query.
void AnnotateWithDevice(std::vector<DeviceMeta> const& devices,
                        std::string const& interfaceId,
                        ChannelInfo& info) {
    if (devices.empty()) {
        return;
    }
    auto const* match = &devices.front();
    for (auto const& candidate : devices) {
        if (candidate.id == interfaceId) {
            match = &candidate;
            break;
        }
    }
    info.deviceName = match->name;
    info.manufacturer = match->manufacturer;
    info.model = match->model;
    if (devices.size() > 1) {
        info.note += (info.note.empty() ? std::string() : std::string(" | ")) +
                     "note: " + std::to_string(devices.size()) +
                     " devices matched this selector";
    }
}

std::string SimpleOrientationName(int value) {
    switch (static_cast<SimpleOrientation>(value)) {
    case SimpleOrientation::NotRotated: return "NotRotated";
    case SimpleOrientation::Rotated90DegreesCounterclockwise: return "Rotated90";
    case SimpleOrientation::Rotated180DegreesCounterclockwise: return "Rotated180";
    case SimpleOrientation::Rotated270DegreesCounterclockwise: return "Rotated270";
    case SimpleOrientation::Faceup: return "Faceup";
    case SimpleOrientation::Facedown: return "Facedown";
    default: return "Unknown";
    }
}

std::string HResultText(winrt::hresult_error const& error) {
    return winrt::to_string(error.message());
}

uint32_t ClampReportInterval(uint32_t desiredMs, uint32_t minimumMs) {
    return std::max(desiredMs, minimumMs);
}

} // namespace

// ==========================================================================
//  Impl
// ==========================================================================
struct SensorManager::Impl {
    std::atomic<bool> active{false};

    // ---- Accelerometer ---------------------------------------------------
    Accelerometer accelerometer{nullptr};
    winrt::event_token accelerometerToken{};
    Channel<Vec3> accelerometerValue;

    // ---- Gyrometer -------------------------------------------------------
    Gyrometer gyrometer{nullptr};
    winrt::event_token gyrometerToken{};
    Channel<Vec3> gyrometerValue;

    // ---- OrientationSensor ----------------------------------------------
    OrientationSensor orientation{nullptr};
    winrt::event_token orientationToken{};
    Channel<OrientationValue> orientationValue;

    // ---- Inclinometer ----------------------------------------------------
    Inclinometer inclinometer{nullptr};
    winrt::event_token inclinometerToken{};
    Channel<InclinationValue> inclinometerValue;

    // ---- Compass ---------------------------------------------------------
    Compass compass{nullptr};
    winrt::event_token compassToken{};
    Channel<double> compassValue;

    // ---- SimpleOrientationSensor ----------------------------------------
    SimpleOrientationSensor simpleOrientation{nullptr};
    winrt::event_token simpleOrientationToken{};
    Channel<int> simpleOrientationValue;

    // ---- HingeAngleSensor -----------------------------------------------
    HingeAngleSensor hingeAngle{nullptr};
    winrt::event_token hingeAngleToken{};
    Channel<double> hingeAngleValue;

    // ----------------------------------------------------------------------
    void StartAccelerometer(uint32_t desiredMs, ChannelInfo& info) {
        try {
            accelerometer = Accelerometer::GetDefault();
        } catch (winrt::hresult_error const& error) {
            info.note = "GetDefault() failed: " + HResultText(error);
            return;
        }
        if (!accelerometer) {
            info.note = "no accelerometer on this machine";
            return;
        }
        info.present = true;
        info.deviceId = winrt::to_string(accelerometer.DeviceId());
        info.minReportIntervalMs = accelerometer.MinimumReportInterval();
        try {
            accelerometer.ReportInterval(ClampReportInterval(desiredMs, info.minReportIntervalMs));
            info.reportIntervalMs = accelerometer.ReportInterval();
        } catch (winrt::hresult_error const& error) {
            info.note = "ReportInterval failed: " + HResultText(error);
        }
        accelerometerToken = accelerometer.ReadingChanged(
            [this](Accelerometer const&, AccelerometerReadingChangedEventArgs const& args) {
                if (!active.load(std::memory_order_relaxed)) {
                    return;
                }
                auto reading = args.Reading();
                Vec3 value;
                value.x = reading.AccelerationX();
                value.y = reading.AccelerationY();
                value.z = reading.AccelerationZ();
                accelerometerValue.Store(value);
            });
        info.eventDriven = true;

        // No parameterless GetDeviceSelector() exists on this class; the
        // reading type is required.
        AnnotateWithDevice(
            QueryDevices(Accelerometer::GetDeviceSelector(AccelerometerReadingType::Standard)),
            info.deviceId, info);
    }

    void StartGyrometer(uint32_t desiredMs, ChannelInfo& info) {
        try {
            gyrometer = Gyrometer::GetDefault();
        } catch (winrt::hresult_error const& error) {
            info.note = "GetDefault() failed: " + HResultText(error);
            return;
        }
        if (!gyrometer) {
            info.note = "no gyrometer on this machine";
            return;
        }
        info.present = true;
        info.deviceId = winrt::to_string(gyrometer.DeviceId());
        info.minReportIntervalMs = gyrometer.MinimumReportInterval();
        try {
            gyrometer.ReportInterval(ClampReportInterval(desiredMs, info.minReportIntervalMs));
            info.reportIntervalMs = gyrometer.ReportInterval();
        } catch (winrt::hresult_error const& error) {
            info.note = "ReportInterval failed: " + HResultText(error);
        }
        gyrometerToken = gyrometer.ReadingChanged(
            [this](Gyrometer const&, GyrometerReadingChangedEventArgs const& args) {
                if (!active.load(std::memory_order_relaxed)) {
                    return;
                }
                auto reading = args.Reading();
                Vec3 value;
                value.x = reading.AngularVelocityX();
                value.y = reading.AngularVelocityY();
                value.z = reading.AngularVelocityZ();
                gyrometerValue.Store(value);
            });
        info.eventDriven = true;

        AnnotateWithDevice(QueryDevices(Gyrometer::GetDeviceSelector()), info.deviceId, info);
    }

    void StartOrientation(uint32_t desiredMs, ChannelInfo& info) {
        try {
            // Absolute readings: fused accel + gyro + magnetometer.  There is
            // no GetDefaultAsync() on this class; GetDefault() is synchronous.
            orientation = OrientationSensor::GetDefault();
        } catch (winrt::hresult_error const& error) {
            info.note = "GetDefault() failed: " + HResultText(error);
            return;
        }
        if (!orientation) {
            info.note = "no fused orientation sensor on this machine";
            return;
        }
        info.present = true;
        info.deviceId = winrt::to_string(orientation.DeviceId());
        info.minReportIntervalMs = orientation.MinimumReportInterval();
        try {
            orientation.ReportInterval(ClampReportInterval(desiredMs, info.minReportIntervalMs));
            info.reportIntervalMs = orientation.ReportInterval();
        } catch (winrt::hresult_error const& error) {
            info.note = "ReportInterval failed: " + HResultText(error);
        }
        orientationToken = orientation.ReadingChanged(
            [this](OrientationSensor const&, OrientationSensorReadingChangedEventArgs const& args) {
                if (!active.load(std::memory_order_relaxed)) {
                    return;
                }
                auto reading = args.Reading();
                OrientationValue value;
                // SensorQuaternion / SensorRotationMatrix expose *methods*
                // (X(), M11(), ...), not public fields.
                auto quaternion = reading.Quaternion();
                value.quaternion.x = quaternion.X();
                value.quaternion.y = quaternion.Y();
                value.quaternion.z = quaternion.Z();
                value.quaternion.w = quaternion.W();
                auto matrix = reading.RotationMatrix();
                value.matrix.m[0] = matrix.M11();
                value.matrix.m[1] = matrix.M12();
                value.matrix.m[2] = matrix.M13();
                value.matrix.m[3] = matrix.M21();
                value.matrix.m[4] = matrix.M22();
                value.matrix.m[5] = matrix.M23();
                value.matrix.m[6] = matrix.M31();
                value.matrix.m[7] = matrix.M32();
                value.matrix.m[8] = matrix.M33();
                orientationValue.Store(value);
            });
        info.eventDriven = true;

        AnnotateWithDevice(
            QueryDevices(OrientationSensor::GetDeviceSelector(SensorReadingType::Absolute)),
            info.deviceId, info);
    }

    void StartInclinometer(uint32_t desiredMs, ChannelInfo& info) {
        try {
            inclinometer = Inclinometer::GetDefault();
        } catch (winrt::hresult_error const& error) {
            info.note = "GetDefault() failed: " + HResultText(error);
            return;
        }
        if (!inclinometer) {
            info.note = "no inclinometer on this machine";
            return;
        }
        info.present = true;
        info.deviceId = winrt::to_string(inclinometer.DeviceId());
        info.minReportIntervalMs = inclinometer.MinimumReportInterval();
        try {
            inclinometer.ReportInterval(ClampReportInterval(desiredMs, info.minReportIntervalMs));
            info.reportIntervalMs = inclinometer.ReportInterval();
        } catch (winrt::hresult_error const& error) {
            info.note = "ReportInterval failed: " + HResultText(error);
        }
        inclinometerToken = inclinometer.ReadingChanged(
            [this](Inclinometer const&, InclinometerReadingChangedEventArgs const& args) {
                if (!active.load(std::memory_order_relaxed)) {
                    return;
                }
                auto reading = args.Reading();
                InclinationValue value;
                // These getters throw E_FAIL on hubs that cannot supply fused
                // angles; the channel then simply stays empty.
                try {
                    value.pitchDeg = reading.PitchDegrees();
                    value.rollDeg = reading.RollDegrees();
                    value.yawDeg = reading.YawDegrees();
                } catch (winrt::hresult_error const&) {
                    return;
                }
                inclinometerValue.Store(value);
            });
        info.eventDriven = true;

        // Inclinometer also requires the reading type.
        AnnotateWithDevice(
            QueryDevices(Inclinometer::GetDeviceSelector(SensorReadingType::Absolute)),
            info.deviceId, info);
    }

    void StartCompass(uint32_t desiredMs, ChannelInfo& info) {
        try {
            compass = Compass::GetDefault();
        } catch (winrt::hresult_error const& error) {
            info.note = "GetDefault() failed: " + HResultText(error);
            return;
        }
        if (!compass) {
            info.note = "no magnetometer/compass on this machine";
            return;
        }
        info.present = true;
        info.deviceId = winrt::to_string(compass.DeviceId());
        info.minReportIntervalMs = compass.MinimumReportInterval();
        try {
            compass.ReportInterval(ClampReportInterval(desiredMs, info.minReportIntervalMs));
            info.reportIntervalMs = compass.ReportInterval();
        } catch (winrt::hresult_error const& error) {
            info.note = "ReportInterval failed: " + HResultText(error);
        }
        compassToken = compass.ReadingChanged(
            [this](Compass const&, CompassReadingChangedEventArgs const& args) {
                if (!active.load(std::memory_order_relaxed)) {
                    return;
                }
                auto reading = args.Reading();
                double heading = 0.0;
                try {
                    heading = reading.HeadingMagneticNorth();
                } catch (winrt::hresult_error const&) {
                    return;
                }
                compassValue.Store(heading);
            });
        info.eventDriven = true;

        AnnotateWithDevice(QueryDevices(Compass::GetDeviceSelector()), info.deviceId, info);
    }

    void StartSimpleOrientation(ChannelInfo& info) {
        try {
            simpleOrientation = SimpleOrientationSensor::GetDefault();
        } catch (winrt::hresult_error const& error) {
            info.note = "GetDefault() failed: " + HResultText(error);
            return;
        }
        if (!simpleOrientation) {
            info.note = "no simple orientation sensor on this machine";
            return;
        }
        info.present = true;
        info.deviceId = winrt::to_string(simpleOrientation.DeviceId());
        try {
            simpleOrientationValue.Store(
                static_cast<int>(simpleOrientation.GetCurrentOrientation()));
        } catch (winrt::hresult_error const&) {
        }
        simpleOrientationToken = simpleOrientation.OrientationChanged(
            [this](SimpleOrientationSensor const&,
                   SimpleOrientationSensorOrientationChangedEventArgs const& args) {
                if (!active.load(std::memory_order_relaxed)) {
                    return;
                }
                simpleOrientationValue.Store(static_cast<int>(args.Orientation()));
            });
        info.eventDriven = true;

        AnnotateWithDevice(QueryDevices(SimpleOrientationSensor::GetDeviceSelector()),
                           info.deviceId, info);
    }

    // HingeAngleSensor is the one API that is expected to return null on this
    // hardware; the diagnostic reports that explicitly instead of faking data.
    void StartHingeAngle(uint32_t desiredMs, ChannelInfo& info) {
        (void)desiredMs;  // this sensor has no ReportInterval
        try {
            hingeAngle = HingeAngleSensor::GetDefaultAsync().get();
        } catch (winrt::hresult_error const& error) {
            info.note = "GetDefaultAsync() failed: " + HResultText(error);
            return;
        }
        if (!hingeAngle) {
            info.note = "unavailable: HingeAngleSensor.GetDefaultAsync() returned null "
                        "(no hinge-angle provider registered by HP/Intel on this machine)";
            return;
        }
        info.present = true;
        info.deviceId = winrt::to_string(hingeAngle.DeviceId());
        // HingeAngleSensor has no ReportInterval/MinimumReportInterval: it is an
        // on-change sensor whose sensitivity is expressed in degrees.
        try {
            info.note = "on-change sensor | report threshold: min " +
                        FormatDouble(hingeAngle.MinReportThresholdInDegrees(), 2) +
                        " deg, current " + FormatDouble(hingeAngle.ReportThresholdInDegrees(), 2) + " deg";
        } catch (winrt::hresult_error const&) {
        }
        hingeAngleToken = hingeAngle.ReadingChanged(
            [this](HingeAngleSensor const&, HingeAngleSensorReadingChangedEventArgs const& args) {
                if (!active.load(std::memory_order_relaxed)) {
                    return;
                }
                hingeAngleValue.Store(args.Reading().AngleInDegrees());
            });
        info.eventDriven = true;

        // Note: HingeAngleSensor.GetDeviceSelector() returns a selector for the
        // *display panels* (it is meant for GetRelatedToAdjacentPanelsAsync),
        // not for the sensor itself, so device naming is intentionally skipped.
    }

    void Stop() {
        active.store(false, std::memory_order_relaxed);
        try {
            if (accelerometer) {
                accelerometer.ReadingChanged(accelerometerToken);
                accelerometer = nullptr;
            }
            if (gyrometer) {
                gyrometer.ReadingChanged(gyrometerToken);
                gyrometer = nullptr;
            }
            if (orientation) {
                orientation.ReadingChanged(orientationToken);
                orientation = nullptr;
            }
            if (inclinometer) {
                inclinometer.ReadingChanged(inclinometerToken);
                inclinometer = nullptr;
            }
            if (compass) {
                compass.ReadingChanged(compassToken);
                compass = nullptr;
            }
            if (simpleOrientation) {
                simpleOrientation.OrientationChanged(simpleOrientationToken);
                simpleOrientation = nullptr;
            }
            if (hingeAngle) {
                hingeAngle.ReadingChanged(hingeAngleToken);
                hingeAngle = nullptr;
            }
        } catch (winrt::hresult_error const&) {
        }
    }
};

// ==========================================================================
//  SensorManager
// ==========================================================================
SensorManager::SensorManager()
    : m_impl(std::make_unique<Impl>()) {}

SensorManager::~SensorManager() {
    Stop();
}

bool SensorManager::Start(uint32_t desiredReportIntervalMs) {
    m_channels.clear();
    m_impl->active.store(true, std::memory_order_relaxed);

    ChannelInfo accelerometerInfo;
    accelerometerInfo.key = "accelerometer";
    accelerometerInfo.label = "Accelerometer";
    m_impl->StartAccelerometer(desiredReportIntervalMs, accelerometerInfo);
    m_channels.push_back(std::move(accelerometerInfo));

    ChannelInfo gyrometerInfo;
    gyrometerInfo.key = "gyrometer";
    gyrometerInfo.label = "Gyrometer";
    m_impl->StartGyrometer(desiredReportIntervalMs, gyrometerInfo);
    m_channels.push_back(std::move(gyrometerInfo));

    ChannelInfo orientationInfo;
    orientationInfo.key = "orientation";
    orientationInfo.label = "OrientationSensor";
    m_impl->StartOrientation(desiredReportIntervalMs, orientationInfo);
    m_channels.push_back(std::move(orientationInfo));

    ChannelInfo inclinometerInfo;
    inclinometerInfo.key = "inclinometer";
    inclinometerInfo.label = "Inclinometer";
    m_impl->StartInclinometer(desiredReportIntervalMs, inclinometerInfo);
    m_channels.push_back(std::move(inclinometerInfo));

    ChannelInfo compassInfo;
    compassInfo.key = "compass";
    compassInfo.label = "Compass";
    m_impl->StartCompass(desiredReportIntervalMs, compassInfo);
    m_channels.push_back(std::move(compassInfo));

    ChannelInfo simpleOrientationInfo;
    simpleOrientationInfo.key = "simple_orientation";
    simpleOrientationInfo.label = "SimpleOrientationSensor";
    m_impl->StartSimpleOrientation(simpleOrientationInfo);
    m_channels.push_back(std::move(simpleOrientationInfo));

    ChannelInfo hingeInfo;
    hingeInfo.key = "hinge_angle";
    hingeInfo.label = "HingeAngleSensor";
    m_impl->StartHingeAngle(desiredReportIntervalMs, hingeInfo);
    m_channels.push_back(std::move(hingeInfo));

    for (auto const& channel : m_channels) {
        if (channel.present && channel.eventDriven) {
            return true;
        }
    }
    return false;
}

void SensorManager::Stop() {
    if (m_impl) {
        m_impl->Stop();
    }
}

SensorSample SensorManager::Snapshot() const {
    SensorSample sample;
    sample.steadySeconds = SteadySeconds();
    sample.wallUnixMs = WallUnixMs();

    auto accel = m_impl->accelerometerValue.Read();
    sample.acceleration = accel.value;
    ApplyStamp(sample.accel, accel);

    auto gyro = m_impl->gyrometerValue.Read();
    sample.angularVelocity = gyro.value;
    ApplyStamp(sample.gyro, gyro);

    auto orientation = m_impl->orientationValue.Read();
    sample.orientation = orientation.value.quaternion;
    sample.rotationMatrix = orientation.value.matrix;
    ApplyStamp(sample.orientationSensor, orientation);

    auto inclination = m_impl->inclinometerValue.Read();
    sample.pitchDeg = inclination.value.pitchDeg;
    sample.rollDeg = inclination.value.rollDeg;
    sample.yawDeg = inclination.value.yawDeg;
    ApplyStamp(sample.inclinometer, inclination);

    auto compass = m_impl->compassValue.Read();
    sample.headingDeg = compass.value;
    ApplyStamp(sample.compass, compass);

    auto simpleOrientation = m_impl->simpleOrientationValue.Read();
    sample.simpleOrientation = simpleOrientation.value;
    ApplyStamp(sample.simpleOrientationChannel, simpleOrientation);

    auto hinge = m_impl->hingeAngleValue.Read();
    sample.hingeAngleDeg = hinge.value;
    ApplyStamp(sample.hingeAngle, hinge);

    return sample;
}

const ChannelInfo* SensorManager::FindChannel(const std::string& key) const {
    for (auto const& channel : m_channels) {
        if (channel.key == key) {
            return &channel;
        }
    }
    return nullptr;
}

std::string SimpleOrientationLabel(int value) {
    return SimpleOrientationName(value);
}

} // namespace dragonfly
