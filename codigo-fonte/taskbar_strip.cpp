#include "taskbar_strip.hpp"

#include <commctrl.h>
#include <gdiplus.h>
#include <initguid.h>
#include <uiautomationclient.h>

#include <algorithm>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace {

constexpr wchar_t strip_class[] = L"BateriaSemFio.TaskbarStrip";
constexpr int item_width = 55;
constexpr int side_padding = 5;
constexpr int strip_height = 30;

template <class Interface>
struct ComReference {
    Interface* value = nullptr;
    ~ComReference() { if (value) value->Release(); }
    Interface* operator->() const { return value; }
};

struct TaskbarSpace {
    HWND bar;
    RECT bounds;
    std::vector<RECT> controls;
    std::optional<LONG> tray_left;
    ULONGLONG measured;
};

bool occupies_space(CONTROLTYPEID type) {
    switch (type) {
    case UIA_ButtonControlTypeId:
    case UIA_CheckBoxControlTypeId:
    case UIA_ComboBoxControlTypeId:
    case UIA_EditControlTypeId:
    case UIA_HyperlinkControlTypeId:
    case UIA_ListItemControlTypeId:
    case UIA_MenuItemControlTypeId:
    case UIA_RadioButtonControlTypeId:
    case UIA_SplitButtonControlTypeId:
    case UIA_TabItemControlTypeId:
    case UIA_TextControlTypeId:
        return true;
    default:
        return false;
    }
}

TaskbarSpace measure_controls(HWND bar, RECT bounds) {
    TaskbarSpace space{bar, bounds, {}, {}, GetTickCount64()};
    ComReference<IUIAutomation2> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IUIAutomation2, reinterpret_cast<void**>(&automation.value))))
        return space;
    automation->put_ConnectionTimeout(900);
    automation->put_TransactionTimeout(900);

    ComReference<IUIAutomationElement> root;
    ComReference<IUIAutomationCondition> condition;
    ComReference<IUIAutomationCacheRequest> cache;
    ComReference<IUIAutomationElementArray> children;
    if (FAILED(automation->ElementFromHandle(bar, &root.value))
        || FAILED(automation->get_ControlViewCondition(&condition.value))
        || FAILED(automation->CreateCacheRequest(&cache.value))) return space;

    for (auto property : {UIA_ControlTypePropertyId, UIA_IsOffscreenPropertyId,
                          UIA_BoundingRectanglePropertyId, UIA_ClassNamePropertyId}) {
        if (FAILED(cache->AddProperty(property))) return space;
    }
    if (FAILED(cache->put_AutomationElementMode(AutomationElementMode_None))
        || FAILED(root->FindAllBuildCache(TreeScope_Descendants, condition.value,
                                           cache.value, &children.value))) return space;

    int length = 0;
    if (FAILED(children->get_Length(&length))) return space;
    for (int i = 0; i < length; ++i) {
        ComReference<IUIAutomationElement> child;
        CONTROLTYPEID type = 0;
        BOOL offscreen = TRUE;
        RECT rect{}, clipped{};
        if (FAILED(children->GetElement(i, &child.value))
            || FAILED(child->get_CachedControlType(&type)) || !occupies_space(type)
            || FAILED(child->get_CachedIsOffscreen(&offscreen)) || offscreen
            || FAILED(child->get_CachedBoundingRectangle(&rect))
            || !IntersectRect(&clipped, &rect, &bounds)) continue;
        space.controls.push_back(clipped);

        BSTR class_name = nullptr;
        if (SUCCEEDED(child->get_CachedClassName(&class_name)) && class_name
            && std::wstring_view(class_name).starts_with(L"SystemTray.")) {
            space.tray_left = std::min(space.tray_left.value_or(bounds.right), clipped.left);
        }
        SysFreeString(class_name);
    }
    return space;
}

bool is_fullscreen(HWND foreground, HWND bar, const RECT& monitor) {
    if (!foreground || foreground == bar || foreground == GetShellWindow()) return false;
    wchar_t name[64]{};
    GetClassNameW(foreground, name, static_cast<int>(std::size(name)));
    const std::wstring_view kind(name);
    if (kind == L"WorkerW" || kind == L"Progman" || kind == L"Shell_SecondaryTrayWnd")
        return false;

    RECT rect{};
    return GetWindowRect(foreground, &rect) && rect.left <= monitor.left
        && rect.top <= monitor.top && rect.right >= monitor.right && rect.bottom >= monitor.bottom;
}

Gdiplus::Color color(COLORREF rgb, BYTE alpha = 255) {
    return {alpha, GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)};
}


void rounded_outline(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float radius) {
    const float d = radius * 2;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void draw_device(Gdiplus::Graphics& graphics, Gdiplus::Pen& pen, BatteryKind kind, float x, float y) {
    Gdiplus::GraphicsPath outline;
    switch (kind) {
    case BatteryKind::mouse:
        rounded_outline(outline, x + 3.0f, y + 0.5f, 10.0f, 14.0f, 4.4f);
        graphics.DrawPath(&pen, &outline);
        graphics.DrawLine(&pen, x + 8.0f, y + 2.6f, x + 8.0f, y + 6.0f);
        break;
    case BatteryKind::keyboard:
        rounded_outline(outline, x + 0.5f, y + 3.0f, 15.0f, 9.0f, 1.8f);
        graphics.DrawPath(&pen, &outline);
        for (int column = 0; column < 3; ++column) {
            const float start = x + 3.3f + column * 3.9f;
            graphics.DrawLine(&pen, start, y + 6.0f, start + 1.0f, y + 6.0f);
        }
        graphics.DrawLine(&pen, x + 4.5f, y + 9.0f, x + 11.5f, y + 9.0f);
        break;
    case BatteryKind::headset:
        graphics.DrawArc(&pen, x + 1.7f, y + 0.8f, 12.6f, 13.5f, 180.0f, 180.0f);
        rounded_outline(outline, x + 1.5f, y + 7.3f, 3.5f, 6.0f, 1.3f);
        outline.StartFigure();
        rounded_outline(outline, x + 11.0f, y + 7.3f, 3.5f, 6.0f, 1.3f);
        graphics.DrawPath(&pen, &outline);
        break;
    case BatteryKind::generic:
        rounded_outline(outline, x + 0.8f, y + 3.8f, 12.5f, 7.5f, 1.4f);
        graphics.DrawPath(&pen, &outline);
        graphics.DrawLine(&pen, x + 15.0f, y + 6.2f, x + 15.0f, y + 9.1f);
        graphics.DrawLine(&pen, x + 3.5f, y + 7.6f, x + 7.6f, y + 7.6f);
        break;
    }
}

} // namespace

struct TaskbarStrip::SpaceProbe {
    std::mutex lock;
    bool pending = false;
    std::optional<TaskbarSpace> last;
};

TaskbarStrip::TaskbarStrip() : probe_(std::make_shared<SpaceProbe>()) {}
TaskbarStrip::~TaskbarStrip() { close(); }

void TaskbarStrip::open(HINSTANCE instance, HWND owner, UINT menu_message) {
    owner_ = owner;
    menu_message_ = menu_message;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&graphics_token_, &input, nullptr) != Gdiplus::Ok) return;

    WNDCLASSW cls{};
    cls.lpfnWndProc = &TaskbarStrip::window_proc;
    cls.hInstance = instance;
    cls.lpszClassName = strip_class;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&cls)) return;

    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                              strip_class, L"Bateria sem fio", WS_POPUP, 0, 0, 1, 1,
                              owner_, nullptr, instance, this);
    if (!window_) return;
    appearance_changed();

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_,
                               nullptr, instance, nullptr);
    if (!tooltip_) return;
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = window_;
    tool.uId = reinterpret_cast<UINT_PTR>(window_);
    tool.lpszText = tooltip_text_.data();
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 350);
}

void TaskbarStrip::close() {
    if (tooltip_ && IsWindow(tooltip_)) DestroyWindow(tooltip_);
    tooltip_ = nullptr;
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    window_ = nullptr;
    if (graphics_token_) Gdiplus::GdiplusShutdown(graphics_token_);
    graphics_token_ = 0;
}

void TaskbarStrip::set_readings(const std::vector<BatteryReading>& readings) {
    readings_ = readings;
    redraw_ = true;
    update_tooltip();
}

void TaskbarStrip::appearance_changed() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    const bool high_contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast),
                                                    &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    DWORD light_theme = 0;
    DWORD size = sizeof(light_theme);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light_theme, &size);
    text_ = high_contrast ? GetSysColor(COLOR_WINDOWTEXT)
                          : (light_theme ? RGB(29, 29, 29) : RGB(246, 246, 246));
    background_ = high_contrast ? GetSysColor(COLOR_WINDOW)
                                : (light_theme ? RGB(248, 248, 248) : RGB(34, 34, 34));
    redraw_ = true;
}

void TaskbarStrip::update_tooltip() {
    if (!tooltip_) return;
    std::wstring text;
    for (const auto& reading : readings_) {
        if (!text.empty()) text += L"\r\n";
        text += reading.name + L" · " + std::to_wstring(reading.percent) + L"%";
        if (reading.from_windows) text += L" · Windows";
    }
    tooltip_text_ = std::move(text);
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND;
    tool.hwnd = window_;
    tool.uId = reinterpret_cast<UINT_PTR>(window_);
    tool.lpszText = tooltip_text_.data();
    SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
}

void TaskbarStrip::probe_space(HWND bar, const RECT& bounds) {
    const auto now = GetTickCount64();
    if (now < next_probe_) return;
    const auto probe = probe_;
    {
        std::lock_guard guard(probe->lock);
        if (probe->pending) return;
        probe->pending = true;
    }
    next_probe_ = now + 1'800;
    try {
        std::thread([probe, bar, bounds]() {
            std::optional<TaskbarSpace> measured;
            if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
                try { measured = measure_controls(bar, bounds); } catch (...) {}
                CoUninitialize();
            }
            std::lock_guard guard(probe->lock);
            probe->last = std::move(measured);
            probe->pending = false;
        }).detach();
    } catch (...) {
        std::lock_guard guard(probe->lock);
        probe->pending = false;
    }
}

bool TaskbarStrip::space_is_free(HWND bar, const RECT& bounds, const RECT& desired) const {
    std::lock_guard guard(probe_->lock);
    const auto& last = probe_->last;
    if (!last || last->bar != bar || !EqualRect(&last->bounds, &bounds) || last->controls.empty()
        || GetTickCount64() - last->measured > 3'800) return false;
    bool saw_task_buttons = false;
    for (const auto& control : last->controls) {
        RECT overlap{};
        if (IntersectRect(&overlap, &control, &desired)) return false;
        if (control.right <= desired.left) saw_task_buttons = true;
    }
    return saw_task_buttons;
}

bool TaskbarStrip::sync() {
    if (!window_) return false;
    const auto hide = [&] {
        if (IsWindowVisible(window_)) ShowWindow(window_, SW_HIDE);
        return false;
    };
    if (readings_.empty()) return hide();

    const HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT bar_rect{};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!bar || !IsWindowVisible(bar) || !GetWindowRect(bar, &bar_rect)
        || !GetMonitorInfoW(MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST), &monitor)
        || bar_rect.right - bar_rect.left <= bar_rect.bottom - bar_rect.top
        || is_fullscreen(GetForegroundWindow(), bar, monitor.rcMonitor)) return hide();

    const UINT dpi = std::max(GetDpiForWindow(bar), 96u);
    const auto scale = [&](int points) { return MulDiv(points, static_cast<int>(dpi), 96); };
    RECT visible{};
    if (!IntersectRect(&visible, &bar_rect, &monitor.rcMonitor)
        || visible.bottom - visible.top < scale(20)) return hide();

    const auto max_count = (visible.right - visible.left - scale(2 * side_padding + 16))
        / scale(item_width);
    if (max_count < 1) return hide();
    const size_t count = std::min(readings_.size(), static_cast<size_t>(max_count));
    const int width = scale(static_cast<int>(count) * item_width + 2 * side_padding);
    const int height = scale(strip_height);
    const int margin = scale(6);

    probe_space(bar, bar_rect);
    std::optional<LONG> tray_left;
    const HWND notification = FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT tray{};
    if (notification && GetWindowRect(notification, &tray)
        && tray.left > visible.left && tray.right <= visible.right && tray.right > tray.left)
        tray_left = tray.left;
    if (!tray_left) {
        std::lock_guard guard(probe_->lock);
        const auto& last = probe_->last;
        if (last && last->bar == bar && EqualRect(&last->bounds, &bar_rect)) tray_left = last->tray_left;
    }

    const LONG right = tray_left.value_or(visible.right) - margin;
    const LONG top = visible.top + (visible.bottom - visible.top - height) / 2;
    RECT desired{right - width, top, right, top + height};
    const bool floating = !tray_left || desired.left < visible.left || desired.top < visible.top
        || desired.bottom > visible.bottom || !space_is_free(bar, bar_rect, desired);
    if (floating) {
        const LONG anchor = visible.right - margin;
        const bool at_top = visible.top - monitor.rcMonitor.top < monitor.rcMonitor.bottom - visible.bottom;
        const LONG edge = at_top ? visible.bottom + margin : visible.top - margin - height;
        desired = {anchor - width, edge, anchor, edge + height};
    }

    if (redraw_ || floating != floating_ || dpi != dpi_ || count != shown_) {
        if (!render(desired, dpi, count, floating)) return hide();
        dpi_ = dpi;
        shown_ = count;
        floating_ = floating;
        redraw_ = false;
    }

    RECT old{};
    if (!IsWindowVisible(window_) || !GetWindowRect(window_, &old) || !EqualRect(&old, &desired)) {
        if (!SetWindowPos(window_, HWND_TOPMOST, desired.left, desired.top, width, height,
                          SWP_NOACTIVATE | SWP_SHOWWINDOW)) return hide();
    }
    return IsWindowVisible(window_);
}

bool TaskbarStrip::render(const RECT& rect, UINT dpi, size_t count, bool floating) {
    using namespace Gdiplus;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    const HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dib) {
        DeleteDC(dc);
        return false;
    }

    const HGDIOBJ previous = SelectObject(dc, dib);
    {
        Bitmap surface(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(pixels));
        Graphics graphics(&surface);
        graphics.Clear(Color(0, 0, 0, 0));
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        const float scale = dpi / 96.0f;
        graphics.ScaleTransform(scale, scale);
        if (floating) {
            GraphicsPath outline;
            rounded_outline(outline, 0.5f, 0.5f, width / scale - 1.0f, height / scale - 1.0f, 8.0f);
            SolidBrush fill(color(background_, 248));
            Pen border(color(text_, 40), 1);
            graphics.FillPath(&fill, &outline);
            graphics.DrawPath(&border, &outline);
        } else {
            // Keep the whitespace clickable without covering the taskbar's appearance.
            SolidBrush hit_area(color(background_, 1));
            graphics.FillRectangle(&hit_area, 0.0f, 0.0f, width / scale, height / scale);
        }

        SolidBrush foreground(color(text_));
        Pen pen(color(text_), 1.15f);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        FontFamily family(L"Segoe UI");
        Font font(family.GetLastStatus() == Ok ? &family : FontFamily::GenericSansSerif(),
                  12.5f, FontStyleRegular, UnitPixel);
        StringFormat format(StringFormat::GenericTypographic());
        format.SetLineAlignment(StringAlignmentCenter);
        format.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
        for (size_t i = 0; i < count; ++i) {
            const float x = side_padding + i * item_width;
            draw_device(graphics, pen, readings_[i].kind, x + 2.0f, 7.5f);
            const auto label = std::to_wstring(readings_[i].percent) + L"%";
            const RectF area(x + 23.0f, 0.0f, 32.0f, static_cast<float>(strip_height));
            graphics.DrawString(label.c_str(), static_cast<INT>(label.size()), &font,
                                area, &format, &foreground);
        }
        graphics.Flush(FlushIntentionSync);
    }
    SIZE size{width, height};
    POINT source{0, 0};
    POINT target{rect.left, rect.top};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL updated = UpdateLayeredWindow(window_, nullptr, &target, &size, dc, &source,
                                             0, &blend, ULW_ALPHA);
    SelectObject(dc, previous);
    DeleteObject(dib);
    DeleteDC(dc);
    return updated;
}

LRESULT CALLBACK TaskbarStrip::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    const auto* strip = reinterpret_cast<const TaskbarStrip*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (strip && (message == WM_LBUTTONUP || message == WM_RBUTTONUP)) {
        PostMessageW(strip->owner_, strip->menu_message_, 0, 0);
        return 0;
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, message, wparam, lparam);
}
