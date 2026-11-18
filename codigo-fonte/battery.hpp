#pragma once

#include <string>
#include <vector>

enum class BatteryKind { generic, mouse, keyboard, headset };

struct BatteryReading {
    std::wstring id;
    std::wstring name;
    unsigned percent;
    bool from_windows;
    BatteryKind kind = BatteryKind::generic;
};

struct BatteryScan {
    std::vector<BatteryReading> readings;
    bool enumeration_failed = false;
};

BatteryScan scan_batteries();
void append_usb_batteries(BatteryScan& scan);
