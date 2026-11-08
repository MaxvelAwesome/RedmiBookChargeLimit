#include "battery.h"

#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#include <winioctl.h>
#include <batclass.h>
#include <cwchar>
#include <vector>

namespace battery {
using app::Handle;

namespace {

std::wstring FirstBatteryPath() {
    HDEVINFO dev = SetupDiGetClassDevsW(&GUID_DEVCLASS_BATTERY, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev == INVALID_HANDLE_VALUE) return {};
    std::wstring path;
    SP_DEVICE_INTERFACE_DATA did{};
    did.cbSize = sizeof(did);
    if (SetupDiEnumDeviceInterfaces(dev, nullptr, &GUID_DEVCLASS_BATTERY, 0, &did)) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(dev, &did, nullptr, 0, &need, nullptr);
        if (need >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            std::vector<BYTE> buf(need);
            auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(dev, &did, det, need, nullptr, nullptr)) path = det->DevicePath;
        }
    }
    SetupDiDestroyDeviceInfoList(dev);
    return path;
}

template <class T>
bool QueryInfo(HANDLE h, ULONG tag, BATTERY_QUERY_INFORMATION_LEVEL level, T& out) {
    BATTERY_QUERY_INFORMATION q{};
    q.BatteryTag = tag;
    q.InformationLevel = level;
    DWORD got = 0;
    return DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof(q), &out, sizeof(out), &got, nullptr) && got > 0;
}

}  // namespace

bool Query(Info& o) {
    o = Info{};

    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps)) {
        o.acOnline = sps.ACLineStatus == 1;
        if (sps.BatteryLifePercent <= 100) o.percent = sps.BatteryLifePercent;
        if (sps.BatteryLifeTime != static_cast<DWORD>(-1)) o.estimatedSec = static_cast<long>(sps.BatteryLifeTime);
        o.present = (sps.BatteryFlag & 128) == 0 && sps.BatteryFlag != 255;
    }

    std::wstring path = FirstBatteryPath();
    if (path.empty()) return o.present;

    Handle h(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h) h.reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h) return o.present;

    ULONG wait = 0, tag = 0;
    DWORD got = 0;
    if (!DeviceIoControl(h.get(), IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &tag, sizeof(tag), &got, nullptr) || tag == 0)
        return o.present;
    o.present = true;

    bool relative = false;
    BATTERY_INFORMATION bi{};
    if (QueryInfo(h.get(), tag, BatteryInformation, bi)) {
        relative = (bi.Capabilities & BATTERY_CAPACITY_RELATIVE) != 0;
        if (!relative) {
            if (bi.DesignedCapacity && bi.DesignedCapacity != BATTERY_UNKNOWN_CAPACITY) o.designMwh = static_cast<long>(bi.DesignedCapacity);
            if (bi.FullChargedCapacity && bi.FullChargedCapacity != BATTERY_UNKNOWN_CAPACITY) o.fullMwh = static_cast<long>(bi.FullChargedCapacity);
        }
        o.cycles = static_cast<long>(bi.CycleCount);
        for (int i = 0; i < 4 && bi.Chemistry[i] >= 0x20 && bi.Chemistry[i] < 0x7F; ++i)
            o.chemistry[i] = static_cast<wchar_t>(bi.Chemistry[i]);
    }

    ULONG temp = 0;
    if (QueryInfo(h.get(), tag, BatteryTemperature, temp) && temp > 2000 && temp < 4000)  // десятые доли Кельвина
        o.temperatureC = temp / 10.0 - 273.15;

    ULONG est = 0;
    if (QueryInfo(h.get(), tag, BatteryEstimatedTime, est) && est != BATTERY_UNKNOWN_TIME && o.estimatedSec < 0)
        o.estimatedSec = static_cast<long>(est);

    wchar_t name[64] = {};
    if (QueryInfo(h.get(), tag, BatteryDeviceName, name)) { name[63] = 0; wcsncpy_s(o.name, name, _TRUNCATE); }
    wchar_t manu[64] = {};
    if (QueryInfo(h.get(), tag, BatteryManufactureName, manu)) { manu[63] = 0; wcsncpy_s(o.manufacturer, manu, _TRUNCATE); }

    BATTERY_WAIT_STATUS ws{};
    ws.BatteryTag = tag;
    BATTERY_STATUS bs{};
    if (DeviceIoControl(h.get(), IOCTL_BATTERY_QUERY_STATUS, &ws, sizeof(ws), &bs, sizeof(bs), &got, nullptr)) {
        o.acOnline = (bs.PowerState & BATTERY_POWER_ON_LINE) != 0;
        o.charging = (bs.PowerState & BATTERY_CHARGING) != 0;
        o.discharging = (bs.PowerState & BATTERY_DISCHARGING) != 0;
        if (bs.Voltage != BATTERY_UNKNOWN_VOLTAGE) o.voltageMv = static_cast<long>(bs.Voltage);
        if (!relative) {
            if (bs.Capacity != BATTERY_UNKNOWN_CAPACITY) o.capacityMwh = static_cast<long>(bs.Capacity);
            if (bs.Rate != static_cast<LONG>(BATTERY_UNKNOWN_RATE)) o.rateMw = bs.Rate;
        }
    }
    return true;
}

}  // namespace battery
