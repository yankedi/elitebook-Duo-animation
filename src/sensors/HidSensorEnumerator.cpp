// ---------------------------------------------------------------------------
//  HidSensorEnumerator.cpp
//
//  Raw HID enumeration / inspection.  Read-only with respect to the device
//  tree; devices are opened with FILE_SHARE_READ | FILE_SHARE_WRITE so the
//  WinRT sensor stack keeps working at the same time.
// ---------------------------------------------------------------------------
#include "HidSensorEnumerator.h"

#include <windows.h>
#include <combaseapi.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cfgmgr32.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dragonfly {

namespace {

// GUID_DEVINTERFACE_HID -- defined locally so this file does not depend on the
// WDK header set.
const GUID kHidInterfaceClass = {
    0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

// Sensor properties -- values verified against the Windows SDK header
// Include/<ver>/shared/sensorsdef.h.  The GUID is {D4247382-969D-4F24-BB14-FB9671870BBF};
// the pids are 2 (Type), 6 (Name), 7 (Manufacturer), 8 (Model), 9 (PersistentUniqueId),
// 10 (VendorDefinedSubType).
const GUID kSensorPropertySetGuid = {
    0xd4247382, 0x969d, 0x4f24, {0xbb, 0x14, 0xfb, 0x96, 0x71, 0x87, 0x0b, 0xbf}};

const DEVPROPKEY kDevPropKeySensorType = {kSensorPropertySetGuid, 2};
const DEVPROPKEY kDevPropKeySensorName = {kSensorPropertySetGuid, 6};
const DEVPROPKEY kDevPropKeySensorManufacturer = {kSensorPropertySetGuid, 7};
const DEVPROPKEY kDevPropKeySensorModel = {kSensorPropertySetGuid, 8};
const DEVPROPKEY kDevPropKeySensorPersistentId = {kSensorPropertySetGuid, 9};
const DEVPROPKEY kDevPropKeySensorVendorSubType = {kSensorPropertySetGuid, 10};

// DEVPKEY_Device_ContainerId -- groups the interfaces of one physical device.
const DEVPROPKEY kDevPropKeyContainerId = {
    {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

// DEVPKEY_Device_FriendlyName
const DEVPROPKEY kDevPropKeyFriendlyName = {
    {0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9f, 0x0b}}, 14};

// DEVPKEY_Device_DeviceDesc
const DEVPROPKEY kDevPropKeyDeviceDesc = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

std::string WideToUtf8(const wchar_t* text) {
    return text ? WideToUtf8(std::wstring(text)) : std::string();
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), size);
    return out;
}

std::string GuidToText(const GUID& guid) {
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               static_cast<unsigned>(guid.Data1),
               static_cast<unsigned>(guid.Data2),
               static_cast<unsigned>(guid.Data3),
               guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
               guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return WideToUtf8(buffer);
}

std::string HexDump(const std::vector<uint8_t>& bytes) {
    std::string out;
    out.reserve(bytes.size() * 3);
    char buffer[8];
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            out += ' ';
        }
        std::snprintf(buffer, sizeof(buffer), "%02X", bytes[i]);
        out += buffer;
    }
    return out;
}

std::string Hex16(uint16_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%04X", value);
    return buffer;
}

std::vector<DeviceInterfaceRef> EnumerateInterfaces(const GUID& classGuid) {
    std::vector<DeviceInterfaceRef> result;

    HDEVINFO set = SetupDiGetClassDevsW(&classGuid, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        return result;
    }

    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(set, nullptr, &classGuid, index, &interfaceData);
         ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, nullptr, 0, &required, nullptr);
        if (required == 0) {
            continue;
        }

        std::vector<BYTE> buffer(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA deviceInfo{};
        deviceInfo.cbSize = sizeof(deviceInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &interfaceData, detail, required,
                                              nullptr, &deviceInfo)) {
            continue;
        }

        DeviceInterfaceRef ref;
        ref.path = WideToUtf8(detail->DevicePath);

        DWORD instanceRequired = 0;
        SetupDiGetDeviceInstanceIdW(set, &deviceInfo, nullptr, 0, &instanceRequired);
        if (instanceRequired > 1) {
            std::wstring instanceId(instanceRequired, L'\0');
            if (SetupDiGetDeviceInstanceIdW(set, &deviceInfo, instanceId.data(),
                                            instanceRequired, nullptr)) {
                instanceId.resize(std::wcslen(instanceId.c_str()));
                ref.instanceId = WideToUtf8(instanceId);
            }
        }
        result.push_back(std::move(ref));
    }

    SetupDiDestroyDeviceInfoList(set);
    return result;
}

std::string ReadDevNodePropertyString(const std::string& instanceId, const DEVPROPKEY& key) {
    const std::wstring wideInstance = Utf8ToWide(instanceId);
    if (wideInstance.empty()) {
        return {};
    }

    DEVINST devInst = 0;
    if (CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(wideInstance.c_str()),
                           CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return {};
    }

    DEVPROPTYPE type = 0;
    ULONG size = 0;
    if (CM_Get_DevNode_PropertyW(devInst, &key, &type, nullptr, &size, 0) != CR_SUCCESS ||
        size == 0) {
        return {};
    }

    std::vector<BYTE> buffer(size);
    if (CM_Get_DevNode_PropertyW(devInst, &key, &type, buffer.data(), &size, 0) != CR_SUCCESS) {
        return {};
    }

    if (type == DEVPROP_TYPE_STRING) {
        return WideToUtf8(reinterpret_cast<const wchar_t*>(buffer.data()));
    }
    if (type == DEVPROP_TYPE_GUID && size >= sizeof(GUID)) {
        GUID guid{};
        std::memcpy(&guid, buffer.data(), sizeof(GUID));
        return GuidToText(guid);
    }
    return {};
}

void AppendValueCaps(HidDeviceInfo& info, PHIDP_PREPARSED_DATA preparsed) {
    USHORT capsLength = 0;
    HidP_GetValueCaps(HidP_Input, nullptr, &capsLength, preparsed);
    if (capsLength == 0) {
        return;
    }

    std::vector<HIDP_VALUE_CAPS> valueCaps(capsLength);
    if (HidP_GetValueCaps(HidP_Input, valueCaps.data(), &capsLength, preparsed) !=
        HIDP_STATUS_SUCCESS) {
        return;
    }

    for (USHORT i = 0; i < capsLength; ++i) {
        const HIDP_VALUE_CAPS& caps = valueCaps[i];
        HidValueCap entry;
        entry.usagePage = caps.UsagePage;
        entry.isRange = caps.IsRange != FALSE;
        entry.usage = entry.isRange ? caps.Range.UsageMin : caps.NotRange.Usage;
        entry.usageMax = entry.isRange ? caps.Range.UsageMax : caps.NotRange.Usage;
        entry.bitSize = caps.BitSize;
        entry.reportCount = caps.ReportCount;
        entry.reportId = caps.ReportID;
        entry.linkCollection = caps.LinkCollection;
        entry.isAbsolute = caps.IsAbsolute != FALSE;
        info.valueCaps.push_back(entry);
    }
}

// Reads one input report (150 ms budget) and decodes every value cap usage.
void ReadSampleReport(HidDeviceInfo& info, HANDLE handle, PHIDP_PREPARSED_DATA preparsed) {
    if (info.inputReportByteLength == 0) {
        return;
    }

    std::vector<uint8_t> report(info.inputReportByteLength, 0);
    DWORD transferred = 0;

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
        return;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event;

    BOOL ok = ReadFile(handle, report.data(), static_cast<DWORD>(report.size()),
                       &transferred, &overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(event, 150) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(handle, &overlapped, &transferred, FALSE);
        } else {
            CancelIo(handle);
            ok = FALSE;
        }
    }
    CloseHandle(event);

    if (!ok || transferred == 0) {
        info.error += (info.error.empty() ? "" : " | ");
        info.error += "no input report within 150 ms";
        return;
    }

    report.resize(transferred);
    info.rawReport = HexDump(report);

    for (const HidValueCap& cap : info.valueCaps) {
        ULONG value = 0;
        const NTSTATUS status = HidP_GetUsageValue(
            HidP_Input, cap.usagePage, cap.linkCollection, cap.usage, &value, preparsed,
            reinterpret_cast<PCHAR>(report.data()), static_cast<ULONG>(report.size()));
        if (status == HIDP_STATUS_SUCCESS) {
            info.parsedUsages.push_back("page " + Hex16(cap.usagePage) + " usage " +
                                        Hex16(cap.usage) + " = " + std::to_string(value));
        }
    }
}

void FillHidDetails(HidDeviceInfo& info, bool readSampleReports) {
    const std::wstring path = Utf8ToWide(info.interfacePath);
    if (path.empty()) {
        info.error = "empty interface path";
        return;
    }

    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        handle = CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    }
    if (handle == INVALID_HANDLE_VALUE) {
        info.error = "CreateFile failed (error " + std::to_string(GetLastError()) + ")";
        return;
    }
    info.opened = true;

    HIDD_ATTRIBUTES attributes{};
    attributes.Size = sizeof(attributes);
    if (HidD_GetAttributes(handle, &attributes)) {
        info.vendorId = attributes.VendorID;
        info.productId = attributes.ProductID;
        info.versionNumber = attributes.VersionNumber;
    }

    wchar_t text[256] = {};
    if (HidD_GetProductString(handle, text, sizeof(text))) {
        info.product = WideToUtf8(text);
    }
    if (HidD_GetManufacturerString(handle, text, sizeof(text))) {
        info.manufacturer = WideToUtf8(text);
    }
    if (HidD_GetSerialNumberString(handle, text, sizeof(text))) {
        info.serialNumber = WideToUtf8(text);
    }

    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetPreparsedData(handle, &preparsed)) {
        info.error += (info.error.empty() ? "" : " | ");
        info.error += "HidD_GetPreparsedData failed";
        CloseHandle(handle);
        return;
    }

    HIDP_CAPS caps{};
    if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
        info.usagePage = caps.UsagePage;
        info.usage = caps.Usage;
        info.inputReportByteLength = caps.InputReportByteLength;
        info.outputReportByteLength = caps.OutputReportByteLength;
        info.featureReportByteLength = caps.FeatureReportByteLength;
        info.isSensorCollection = (caps.UsagePage == 0x20);
    }

    AppendValueCaps(info, preparsed);

    if (readSampleReports && info.isSensorCollection) {
        ReadSampleReport(info, handle, preparsed);
    }

    HidD_FreePreparsedData(preparsed);
    CloseHandle(handle);
}

} // namespace

namespace {

std::vector<HidDeviceInfo> InspectInterfaces(const std::vector<DeviceInterfaceRef>& refs,
                                             bool readSampleReports,
                                             const std::string& sourceGroup) {
    std::vector<HidDeviceInfo> devices;

    for (const DeviceInterfaceRef& ref : refs) {
        HidDeviceInfo info;
        info.interfacePath = ref.path;
        info.instanceId = ref.instanceId;
        info.sourceGroup = sourceGroup;
        info.containerId = ReadDevNodePropertyString(ref.instanceId, kDevPropKeyContainerId);
        info.sensorTypeGuid = ReadDevNodePropertyString(ref.instanceId, kDevPropKeySensorType);
        info.sensorName = ReadDevNodePropertyString(ref.instanceId, kDevPropKeySensorName);
        info.sensorManufacturer =
            ReadDevNodePropertyString(ref.instanceId, kDevPropKeySensorManufacturer);
        info.sensorModel = ReadDevNodePropertyString(ref.instanceId, kDevPropKeySensorModel);
        info.sensorPersistentId =
            ReadDevNodePropertyString(ref.instanceId, kDevPropKeySensorPersistentId);
        info.sensorVendorSubType =
            ReadDevNodePropertyString(ref.instanceId, kDevPropKeySensorVendorSubType);

        const std::string friendlyName =
            ReadDevNodePropertyString(ref.instanceId, kDevPropKeyFriendlyName);
        const std::string description =
            ReadDevNodePropertyString(ref.instanceId, kDevPropKeyDeviceDesc);

        FillHidDetails(info, readSampleReports);

        if (info.product.empty()) {
            if (!info.sensorName.empty()) {
                info.product = info.sensorName;
            } else if (!friendlyName.empty()) {
                info.product = friendlyName;
            } else {
                info.product = description;
            }
        }
        devices.push_back(std::move(info));
    }

    return devices;
}

} // namespace

std::vector<HidDeviceInfo> EnumerateHidDevices(bool readSampleReports) {
    return InspectInterfaces(EnumerateInterfaces(kHidInterfaceClass), readSampleReports, "HID");
}

std::vector<HidDeviceInfo> EnumerateInterfacesAsHidDevices(
    const std::string& interfaceClassGuidText, bool readSampleReports) {
    return InspectInterfaces(EnumerateInterfacesByGuidText(interfaceClassGuidText),
                             readSampleReports, interfaceClassGuidText);
}

std::vector<DeviceInterfaceRef> EnumerateInterfacesByGuidText(const std::string& guidText) {
    std::string text = guidText;
    if (text.empty()) {
        return {};
    }
    if (text.front() != '{') {
        text = "{" + text + "}";
    }
    const std::wstring wide = Utf8ToWide(text);
    GUID guid{};
    if (FAILED(CLSIDFromString(wide.c_str(), &guid))) {
        return {};
    }
    return EnumerateInterfaces(guid);
}

} // namespace dragonfly
