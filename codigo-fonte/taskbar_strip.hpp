#pragma once

#include "battery.hpp"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

class TaskbarStrip {
public:
    TaskbarStrip();
    ~TaskbarStrip();

    void open(HINSTANCE instance, HWND owner, UINT menu_message);
    void close();
    void set_readings(const std::vector<BatteryReading>& readings);
    void appearance_changed();
    bool sync();

private:
    struct SpaceProbe;

    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    HWND tooltip_ = nullptr;
    UINT menu_message_ = 0;
    ULONG_PTR graphics_token_ = 0;
    ULONGLONG next_probe_ = 0;
    std::shared_ptr<SpaceProbe> probe_;
    std::vector<BatteryReading> readings_;
    std::wstring tooltip_text_;
    COLORREF text_ = RGB(245, 245, 245);
    COLORREF background_ = RGB(32, 32, 32);
    bool redraw_ = true;
    bool floating_ = false;
    UINT dpi_ = 96;
    size_t shown_ = 0;

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void probe_space(HWND bar, const RECT& bounds);
    bool space_is_free(HWND bar, const RECT& bounds, const RECT& desired) const;
    void update_tooltip();
    bool render(const RECT& rect, UINT dpi, size_t count, bool floating);
};
