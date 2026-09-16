// ---------------------------------------------------------------------------
//  HidSensorEnumerator.h
//
//  Native Win32 channel: enumerates HID device interfaces directly through
//  SetupAPI + the HID parser, independently of WinRT.
//
//  Why both channels exist:
//    * WinRT CustomSensor is the supported way to read the Intel Lid Mode
//      sensor, but it only sees devices the sensor stack registered.
//    * The raw HID view proves what the device tree actually contains, exposes
//      the HID value caps and can dump a raw input report -- which is the only
//      way to identify a vendor-defined usage the WinRT layer hides.
//
//  Nothing here is Intel-specific; every device found is reported truthfully.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dragonfly {

struct HidValueCap {
    uint16_t usagePage = 0;
    uint16_t usage = 0;          // NotRange.Usage, or Range.UsageMin for ranges
    uint16_t usageMax = 0;
    bool isRange = false;
    uint16_t bitSize = 0;
    uint16_t reportCount = 0;
    uint16_t reportId = 0;
    uint16_t linkCollection = 0;
    bool isAbsolute = false;
};

struct HidDeviceInfo {
    std::string interfacePath;   // \\?\hid#vid_8087&pid_0ac2#...#{...}
    std::string instanceId;      // HID\VID_8087&PID_0AC2\6&1201cbe5&0&0000
    std::string containerId;     // groups interfaces of one physical device

    // Official sensor properties (Windows SDK shared\sensorsdef.h,
    // DEVPKEY_Sensor_*, {D4247382-969D-4F24-BB14-FB9671870BBF}).
    std::string sensorTypeGuid;        // pid 2,  VT_CLSID   -- the logical sensor
    std::string sensorName;            // pid 6,  VT_LPWSTR
    std::string sensorManufacturer;    // pid 7,  VT_LPWSTR
    std::string sensorModel;           // pid 8,  VT_LPWSTR
    std::string sensorPersistentId;    // pid 9,  VT_CLSID
    std::string sensorVendorSubType;   // pid 10, VT_CLSID

    std::string product;
    std::string manufacturer;
    std::string serialNumber;

    uint16_t vendorId = 0;
    uint16_t productId = 0;
    uint16_t versionNumber = 0;

    uint16_t usagePage = 0;
    uint16_t usage = 0;
    uint32_t inputReportByteLength = 0;
    uint32_t outputReportByteLength = 0;
    uint32_t featureReportByteLength = 0;

    std::vector<HidValueCap> valueCaps;

    bool isSensorCollection = false;   // usage page 0x20 (Sensor)
    bool opened = false;
    std::string error;

    std::string sourceGroup;           // which interface class produced this entry

    std::string rawReport;             // hex dump of one input report
    std::vector<std::string> parsedUsages;  // "page 0xXXXX usage 0xXXXX = value"
};

// Enumerates every present HID device interface.  When `readSampleReports` is
// true, opens the sensor-page devices and reads one input report (150 ms
// timeout each) plus a best-effort usage decode.
std::vector<HidDeviceInfo> EnumerateHidDevices(bool readSampleReports);

struct DeviceInterfaceRef {
    std::string path;
    std::string instanceId;
};

// Enumerates the present device interfaces of an arbitrary interface class,
// given as "00000300-766d-4333-8262-27e82dd158b1" (braces optional).  Used to
// verify GUIDs recorded in the research notes against the live machine.
std::vector<DeviceInterfaceRef> EnumerateInterfacesByGuidText(const std::string& guidText);

// Same inspection as EnumerateHidDevices(), but for an arbitrary interface
// class.  The Intel Sensor Hub registers its collections under a private
// interface class GUID instead of GUID_DEVINTERFACE_HID, so this is the only
// way to reach them raw.
std::vector<HidDeviceInfo> EnumerateInterfacesAsHidDevices(const std::string& interfaceClassGuidText,
                                                            bool readSampleReports);

// Well known interface class GUIDs, as text.
constexpr const char* kHidInterfaceClassGuidText =
    "4d1e55b2-f16f-11cf-88cb-001111000030";
constexpr const char* kSensorInterfaceClassGuidText =
    "ba1bb692-9b7a-4833-9a1e-525ed134e7e2";
constexpr const char* kLidModeInterfaceClassGuidText =
    "00000300-766d-4333-8262-27e82dd158b1";

} // namespace dragonfly
