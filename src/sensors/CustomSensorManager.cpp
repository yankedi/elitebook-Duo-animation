// ---------------------------------------------------------------------------
//  CustomSensorManager.cpp
//
//  See the header for the two enumeration paths.  Nothing here assumes the
//  Lid Mode sensor exists: every step is verified against the live device tree
//  and every failure is reported verbatim.
// ---------------------------------------------------------------------------
#include "CustomSensorManager.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Sensors.Custom.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dragonfly {

namespace {

using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Sensors::Custom::CustomSensor;
using winrt::Windows::Devices::Sensors::Custom::CustomSensorReading;
using winrt::Windows::Devices::Sensors::Custom::CustomSensorReadingChangedEventArgs;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::IPropertyValue;
using winrt::Windows::Foundation::PropertyType;

// --------------------------------------------------------------------------
//  Candidates recorded in the project's own research notes.  They are used to
//  *ask the machine*, never to fabricate a result: if the device tree does not
//  match, the enumeration output says so.
// --------------------------------------------------------------------------
winrt::guid MakeGuid(uint32_t data1, uint16_t data2, uint16_t data3,
                     uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                     uint8_t e, uint8_t f, uint8_t g, uint8_t h) {
    winrt::guid value{};
    value.Data1 = data1;
    value.Data2 = data2;
    value.Data3 = data3;
    value.Data4[0] = a;
    value.Data4[1] = b;
    value.Data4[2] = c;
    value.Data4[3] = d;
    value.Data4[4] = e;
    value.Data4[5] = f;
    value.Data4[6] = g;
    value.Data4[7] = h;
    return value;
}

// Section 7 of HP_Elite_Dragonfly_G2_Hinge_Sensor_Research.md: the sensor-type
// interface GUID that addressed the Intel Lid Mode Sensor via CustomSensor.
winrt::guid LidModeSensorTypeGuid() {
    return MakeGuid(0x00000300, 0x766d, 0x4333,
                    0x82, 0x62, 0x27, 0xe8, 0x2d, 0xd1, 0x58, 0xb1);
}

// Section 9: the property key that carried the UInt32 state code 1..5.
// Note the keys in CustomSensorReading.Properties() are *strings* of the form
// "{c458f8a7-4ae8-4777-9607-2e9bdd65110a} 161" -- the GUID plus the PROPERTYKEY
// pid -- so matching is done on the text, not on a guid type.
winrt::guid LidModePropertyGuid() {
    return MakeGuid(0xc458f8a7, 0x4ae8, 0x4777,
                    0x96, 0x07, 0x2e, 0x9b, 0xdd, 0x65, 0x11, 0x0a);
}

// {BA1BB692-9B7A-4833-9A1E-525ED134E7E2} == GUID_DEVINTERFACE_SENSOR, from the
// Windows SDK (um\sensors.h).  DeviceClass has no Sensor member, so the AQS
// form is the only way to enumerate sensor device interfaces.
const wchar_t* kSensorInterfaceAqs =
    L"System.Devices.InterfaceClassGuid:=\"{BA1BB692-9B7A-4833-9A1E-525ED134E7E2}\"";

// The recorded Lid Mode property, lower-cased for text matching.
const char* kLidModeKeyGuidFragment = "c458f8a7";
const char* kLidModeKeyPidFragment = "161";

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool MentionsLid(std::string const& name) {
    return ToLower(name).find("lid") != std::string::npos;
}

struct DescribedValue {
    std::string text;
    std::string typeName;
    bool isUInt32 = false;
    uint32_t unsignedValue = 0;
};

DescribedValue DescribeValue(IInspectable const& value) {
    DescribedValue out;
    if (!value) {
        out.typeName = "<null>";
        return out;
    }
    if (auto propertyValue = value.try_as<IPropertyValue>()) {
        switch (propertyValue.Type()) {
        case PropertyType::UInt32:
            out.unsignedValue = propertyValue.GetUInt32();
            out.isUInt32 = true;
            out.text = std::to_string(out.unsignedValue);
            out.typeName = "UInt32";
            break;
        case PropertyType::Int32:
            out.text = std::to_string(propertyValue.GetInt32());
            out.typeName = "Int32";
            break;
        case PropertyType::UInt64:
            out.text = std::to_string(propertyValue.GetUInt64());
            out.typeName = "UInt64";
            break;
        case PropertyType::Int64:
            out.text = std::to_string(propertyValue.GetInt64());
            out.typeName = "Int64";
            break;
        case PropertyType::Double:
            out.text = FormatDouble(propertyValue.GetDouble(), 6);
            out.typeName = "Double";
            break;
        case PropertyType::Single:
            out.text = FormatDouble(static_cast<double>(propertyValue.GetSingle()), 6);
            out.typeName = "Single";
            break;
        case PropertyType::Boolean:
            out.text = propertyValue.GetBoolean() ? "true" : "false";
            out.typeName = "Boolean";
            break;
        case PropertyType::String:
            out.text = winrt::to_string(propertyValue.GetString());
            out.typeName = "String";
            break;
        case PropertyType::DateTime: {
            auto timestamp = propertyValue.GetDateTime();
            out.text = std::to_string(timestamp.time_since_epoch().count());
            out.typeName = "DateTime(100ns ticks)";
            break;
        }
        case PropertyType::TimeSpan: {
            auto span = propertyValue.GetTimeSpan();
            out.text = std::to_string(span.count());
            out.typeName = "TimeSpan(100ns ticks)";
            break;
        }
        case PropertyType::Guid:
            out.text = winrt::to_string(winrt::to_hstring(propertyValue.GetGuid()));
            out.typeName = "Guid";
            break;
        default:
            out.typeName = "PropertyType(" +
                           std::to_string(static_cast<int>(propertyValue.Type())) + ")";
            break;
        }
        return out;
    }
    // SensorData did not expose IPropertyValue: record what it actually is so
    // the raw dump can still be analysed offline.
    try {
        out.typeName = winrt::to_string(winrt::get_class_name(value));
    } catch (winrt::hresult_error const&) {
        out.typeName = "<unknown>";
    }
    return out;
}

bool IsStateLike(uint32_t value) {
    return value >= 1 && value <= 5;
}

// The Lid Mode property key text, e.g. "{c458f8a7-4ae8-4777-9607-2e9bdd65110a} 161".
bool IsRecordedLidModeKey(const std::string& key) {
    const std::string lower = ToLower(key);
    return lower.find(kLidModeKeyGuidFragment) != std::string::npos &&
           lower.find(kLidModeKeyPidFragment) != std::string::npos;
}

bool IsLidModeGuidKey(const std::string& key) {
    return ToLower(key).find(kLidModeKeyGuidFragment) != std::string::npos;
}

struct ReadingDump {
    std::vector<CustomPropertyDump> properties;
    bool hasStateCandidate = false;
    uint32_t stateValue = 0;
    std::string stateKey;
};

ReadingDump DumpReading(CustomSensorReading const& reading) {
    ReadingDump dump;
    std::vector<std::pair<std::string, uint32_t>> unsignedValues;

    // Properties() is IMapView<hstring, IInspectable>: the key is the textual
    // form of the driver PROPERTYKEY ("{guid} pid"), the value is boxed.
    for (auto const& pair : reading.Properties()) {
        const std::string key = winrt::to_string(pair.Key());
        DescribedValue described = DescribeValue(pair.Value());

        CustomPropertyDump entry;
        entry.key = key;
        entry.value = described.text;
        entry.typeName = described.typeName;
        dump.properties.push_back(entry);

        if (described.isUInt32) {
            unsignedValues.emplace_back(key, described.unsignedValue);
        }
    }

    // Preference order for the state code:
    //   1. the recorded property key ({c458f8a7-...} 161) with a state value
    //   2. the recorded property key, any value
    //   3. the recorded GUID with a state value
    //   4. any UInt32 in 1..5
    for (auto const& entry : unsignedValues) {
        if (IsRecordedLidModeKey(entry.first) && IsStateLike(entry.second)) {
            dump.hasStateCandidate = true;
            dump.stateValue = entry.second;
            dump.stateKey = entry.first;
            return dump;
        }
    }
    for (auto const& entry : unsignedValues) {
        if (IsRecordedLidModeKey(entry.first)) {
            dump.hasStateCandidate = true;
            dump.stateValue = entry.second;
            dump.stateKey = entry.first;
            return dump;
        }
    }
    for (auto const& entry : unsignedValues) {
        if (IsLidModeGuidKey(entry.first) && IsStateLike(entry.second)) {
            dump.hasStateCandidate = true;
            dump.stateValue = entry.second;
            dump.stateKey = entry.first;
            return dump;
        }
    }
    for (auto const& entry : unsignedValues) {
        if (IsStateLike(entry.second)) {
            dump.hasStateCandidate = true;
            dump.stateValue = entry.second;
            dump.stateKey = entry.first;
            return dump;
        }
    }
    return dump;
}

std::vector<CustomSensorCandidate> CollectCandidates(bool openThem) {
    std::vector<CustomSensorCandidate> result;
    std::vector<std::string> seen;

    auto push = [&](DeviceInformation const& device, bool lidCandidate,
                    bool fromSensorTypeSelector) {
        auto id = winrt::to_string(device.Id());
        auto existing = std::find(seen.begin(), seen.end(), id);
        if (existing != seen.end()) {
            auto& entry = result[static_cast<size_t>(existing - seen.begin())];
            entry.lidCandidate = entry.lidCandidate || lidCandidate;
            entry.fromSensorTypeSelector = entry.fromSensorTypeSelector || fromSensorTypeSelector;
            return;
        }
        seen.push_back(id);

        CustomSensorCandidate candidate;
        candidate.id = std::move(id);
        candidate.name = winrt::to_string(device.Name());
        candidate.lidCandidate = lidCandidate;
        candidate.fromSensorTypeSelector = fromSensorTypeSelector;

        try {
            auto properties = device.Properties();
            if (properties) {
                if (properties.HasKey(L"System.Devices.Manufacturer")) {
                    if (auto value = properties.Lookup(L"System.Devices.Manufacturer")
                                          .try_as<IPropertyValue>()) {
                        if (value.Type() == PropertyType::String) {
                            candidate.manufacturer = winrt::to_string(value.GetString());
                        }
                    }
                }
                if (properties.HasKey(L"System.Devices.ModelName")) {
                    if (auto value = properties.Lookup(L"System.Devices.ModelName")
                                          .try_as<IPropertyValue>()) {
                        if (value.Type() == PropertyType::String) {
                            candidate.model = winrt::to_string(value.GetString());
                        }
                    }
                }
            }
        } catch (winrt::hresult_error const&) {
        }

        result.push_back(std::move(candidate));
    };

    // Path 1: the sensor-type interface GUID from the research notes.
    try {
        auto selector = CustomSensor::GetDeviceSelector(LidModeSensorTypeGuid());
        auto devices = DeviceInformation::FindAllAsync(selector).get();
        for (auto const& device : devices) {
            push(device, MentionsLid(winrt::to_string(device.Name())), true);
        }
    } catch (winrt::hresult_error const&) {
    }

    // Path 2: every sensor device interface (GUID_DEVINTERFACE_SENSOR), so the
    // dump also proves what the hub exposes beyond the recorded GUID.
    try {
        auto devices = DeviceInformation::FindAllAsync(kSensorInterfaceAqs).get();
        for (auto const& device : devices) {
            auto name = winrt::to_string(device.Name());
            push(device, MentionsLid(name), false);
        }
    } catch (winrt::hresult_error const&) {
    }

    if (!openThem) {
        return result;
    }

    // Try to open every lid candidate (plus everything the recorded GUID
    // matched) and capture one reading from each.
    for (auto& candidate : result) {
        if (!candidate.lidCandidate && !candidate.fromSensorTypeSelector) {
            continue;
        }
        try {
            auto sensor = CustomSensor::FromIdAsync(winrt::to_hstring(candidate.id)).get();
            if (!sensor) {
                candidate.error = "FromIdAsync returned null";
                continue;
            }
            candidate.openable = true;
            try {
                auto reading = sensor.GetCurrentReading();
                auto dump = DumpReading(reading);
                candidate.firstReading = std::move(dump.properties);
            } catch (winrt::hresult_error const& error) {
                candidate.error = "GetCurrentReading failed: " +
                                  winrt::to_string(error.message());
            }
        } catch (winrt::hresult_error const& error) {
            candidate.error = "FromIdAsync failed: " + winrt::to_string(error.message());
        }
    }

    return result;
}

} // namespace

// ==========================================================================
//  Impl
// ==========================================================================
struct CustomSensorManager::Impl {
    std::atomic<bool> active{false};

    CustomSensor sensor{nullptr};
    winrt::event_token token{};

    mutable std::mutex mutex;
    bool hasReading = false;
    uint32_t rawValue = 0;
    double steadySeconds = 0.0;
    LidModeInfo info;

    void ApplyReading(CustomSensorReading const& reading) {
        ReadingDump dump = DumpReading(reading);
        std::lock_guard<std::mutex> lock(mutex);
        info.lastProperties = dump.properties;
        if (dump.hasStateCandidate) {
            rawValue = dump.stateValue;
            info.rawValue = static_cast<int>(dump.stateValue);
            info.valueKey = dump.stateKey;
            hasReading = true;
            steadySeconds = SteadySeconds();
            info.reading = true;
        } else if (!dump.properties.empty()) {
            // The driver answered but no state-like value was recognised; keep
            // the dump so it can be analysed, but do not invent a value.
            info.reading = false;
        }
    }

    bool TryOpen(std::string const& deviceId, std::string const& deviceName,
                 uint32_t desiredMs, bool fallback) {
        try {
            sensor = CustomSensor::FromIdAsync(winrt::to_hstring(deviceId)).get();
        } catch (winrt::hresult_error const&) {
            return false;
        }
        if (!sensor) {
            return false;
        }

        info.present = true;
        info.usedFallbackPath = fallback;
        info.deviceId = winrt::to_string(sensor.DeviceId());
        info.deviceName = deviceName;

        try {
            info.minReportIntervalMs = sensor.MinimumReportInterval();
            uint32_t want = std::max(desiredMs, info.minReportIntervalMs);
            sensor.ReportInterval(want);
            info.reportIntervalMs = sensor.ReportInterval();
        } catch (winrt::hresult_error const& error) {
            info.note += (info.note.empty() ? "" : " | ");
            info.note += "ReportInterval: " + winrt::to_string(error.message());
        }
        try {
            info.maxBatchSize = sensor.MaxBatchSize();
        } catch (winrt::hresult_error const&) {
        }

        // On-change sensors only fire on change, so seed the value once.
        try {
            ApplyReading(sensor.GetCurrentReading());
        } catch (winrt::hresult_error const& error) {
            info.note += (info.note.empty() ? "" : " | ");
            info.note += "GetCurrentReading: " + winrt::to_string(error.message());
        }

        token = sensor.ReadingChanged(
            [this](CustomSensor const&, CustomSensorReadingChangedEventArgs const& args) {
                if (!active.load(std::memory_order_relaxed)) {
                    return;
                }
                try {
                    ApplyReading(args.Reading());
                } catch (winrt::hresult_error const&) {
                }
            });
        return true;
    }

    std::vector<CustomSensorCandidate> EnumerateLidCandidates(bool openThem) {
        return CollectCandidates(openThem);
    }

    bool Start(uint32_t desiredMs) {
        active.store(true, std::memory_order_relaxed);
        auto candidates = CollectCandidates(false);

        for (auto const& candidate : candidates) {
            if (!candidate.lidCandidate) {
                continue;
            }
            if (TryOpen(candidate.id, candidate.name, desiredMs, false)) {
                return true;
            }
        }
        for (auto const& candidate : candidates) {
            if (!candidate.fromSensorTypeSelector) {
                continue;
            }
            if (TryOpen(candidate.id, candidate.name, desiredMs, true)) {
                return true;
            }
        }
        info.note += (info.note.empty() ? "" : " | ");
        if (candidates.empty()) {
            info.note += "no custom-sensor device matched the recorded sensor-type GUID "
                         "or advertised a lid-mode name";
        } else {
            info.note += "candidates were found but none could be opened as a CustomSensor";
        }
        return false;
    }

    void Stop() {
        active.store(false, std::memory_order_relaxed);
        try {
            if (sensor) {
                sensor.ReadingChanged(token);
                sensor = nullptr;
            }
        } catch (winrt::hresult_error const&) {
        }
    }
};

// ==========================================================================
//  CustomSensorManager
// ==========================================================================
CustomSensorManager::CustomSensorManager()
    : m_impl(std::make_unique<Impl>()) {}

CustomSensorManager::~CustomSensorManager() {
    Stop();
}

std::vector<CustomSensorCandidate> CustomSensorManager::Enumerate() {
    return m_impl->EnumerateLidCandidates(true);
}

bool CustomSensorManager::Start(uint32_t desiredReportIntervalMs) {
    return m_impl->Start(desiredReportIntervalMs);
}

void CustomSensorManager::Stop() {
    if (m_impl) {
        m_impl->Stop();
    }
}

int CustomSensorManager::LidMode() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->hasReading ? static_cast<int>(m_impl->rawValue) : -1;
}

ChannelStamp CustomSensorManager::LidModeStamp() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    ChannelStamp stamp;
    stamp.valid = m_impl->hasReading;
    stamp.steadySeconds = m_impl->steadySeconds;
    return stamp;
}

LidModeInfo CustomSensorManager::Info() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->info;
}

} // namespace dragonfly
