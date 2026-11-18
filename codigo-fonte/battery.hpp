#pragma once

#include <string>
#include <vector>

struct BatteryReading {
    std::wstring id;
    std::wstring name;
    unsigned percent;
    bool from_windows;
};

struct BatteryScan {
    std::vector<BatteryReading> readings;
    bool enumeration_failed = false;
};

BatteryScan scan_batteries();
