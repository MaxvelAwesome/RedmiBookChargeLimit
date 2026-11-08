// Метрики батареи через IOCTL_BATTERY_* (работает без прав администратора).
#pragma once

#include "common.h"

namespace battery {

struct Info {
    bool  present = false;
    bool  acOnline = false;
    bool  charging = false;
    bool  discharging = false;
    int   percent = -1;           // как в Windows (0..100)
    long  rateMw = 0;             // >0 зарядка, <0 разрядка, мВт
    long  voltageMv = -1;
    long  capacityMwh = -1;       // текущая энергия
    long  fullMwh = -1;           // полная ёмкость
    long  designMwh = -1;         // проектная ёмкость
    long  cycles = -1;
    double temperatureC = -1000;  // если драйвер отдаёт
    long  estimatedSec = -1;      // оценка Windows на батарее
    wchar_t chemistry[8] = {};
    wchar_t name[64] = {};
    wchar_t manufacturer[64] = {};
};

// Опрашивает первую батарею системы. Безопасно вызывать из любого потока.
bool Query(Info& out);

}  // namespace battery
