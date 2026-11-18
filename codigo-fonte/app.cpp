#include "battery.hpp"
#include "taskbar_strip.hpp"

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace {

constexpr wchar_t app_name[] = L"Bateria sem fio";
constexpr wchar_t window_class[] = L"BateriaSemFio.Tray.35191DD2";
constexpr wchar_t instance_mutex[] = L"Local\\BateriaSemFio.35191DD2";
constexpr wchar_t empty_id[] = L"BateriaSemFio";
constexpr UINT tray_message = WM_APP + 1;
constexpr UINT scan_message = WM_APP + 2;
constexpr UINT menu_message = WM_APP + 3;
constexpr UINT refresh_command = 1;
constexpr UINT quit_command = 2;
constexpr UINT_PTR timer_id = 1;
constexpr UINT_PTR placement_timer_id = 2;
constexpr UINT refresh_ms = 60'000;
constexpr UINT placement_ms = 600;

constexpr std::array<std::array<uint8_t, 7>, 10> digits{{
    {7, 5, 5, 5, 5, 5, 7}, {2, 6, 2, 2, 2, 2, 7},
    {7, 1, 1, 7, 4, 4, 7}, {7, 1, 1, 7, 1, 1, 7},
    {5, 5, 5, 7, 1, 1, 1}, {7, 4, 4, 7, 1, 1, 7},
    {7, 4, 4, 7, 5, 5, 7}, {7, 1, 1, 2, 2, 4, 4},
    {7, 5, 5, 7, 5, 5, 7}, {7, 5, 5, 7, 1, 1, 7},
}};

uint32_t icon_pixel(int x, int y, std::optional<unsigned> percent, std::wstring_view number) {
    const bool corner = (x < 2 || x > 13) && (y < 2 || y > 13);
    if (corner && (x == 0 || x == 15 || y == 0 || y == 15)) return 0;

    if (!percent) {
        const bool body = x >= 2 && x <= 12 && y >= 5 && y <= 10;
        const bool terminal = x == 13 && (y == 7 || y == 8);
        if (terminal || (body && (x == 2 || x == 12 || y == 5 || y == 10 || x <= 7)))
            return 0xffd6f5ff;
        return 0xff224967;
    }

    constexpr uint32_t background = 0xff303030;
    const int length = static_cast<int>(number.size());
    const int width = length == 3 ? 3 : 4;
    const int start = (16 - (length * (width + 1) - 1)) / 2;
    const int offset = x - start;
    if (y >= 3 && y <= 11 && offset >= 0) {
        const int index = offset / (width + 1);
        const int column = offset % (width + 1);
        if (index < length && column < width) {
            const int digit = number[index] - L'0';
            const int bit = 2 - column * 3 / width;
            const int row = (y - 3) * 7 / 9;
            if (digits[digit][row] & (1 << bit)) return 0xfff5f5f5;
        }
    }
    if (y == 13 && x >= 2 && x <= 13) {
        const auto fill = (12 * *percent + 99) / 100;
        return static_cast<unsigned>(x - 2) < fill ? 0xffdfdfdf : 0xff545454;
    }
    return background;
}

HICON make_icon(std::optional<unsigned> percent) {
    const int size = std::clamp(GetSystemMetrics(SM_CXSMICON), 16, 64);
    const std::wstring number = percent ? std::to_wstring(*percent) : L"";

    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = size;
    bitmap.bmiHeader.biHeight = -size;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    const HBITMAP color = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!color) return nullptr;

    const int mask_stride = ((size + 15) / 16) * 2;
    std::array<BYTE, 512> mask_bits{};
    auto* values = static_cast<uint32_t*>(pixels);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const auto value = icon_pixel(x * 16 / size, y * 16 / size, percent, number);
            values[y * size + x] = value;
            if (!value) {
                mask_bits[(size - y - 1) * mask_stride + x / 8] |= static_cast<BYTE>(0x80 >> (x % 8));
            }
        }
    }
    const HBITMAP mask = CreateBitmap(size, size, 1, 1, mask_bits.data());
    HICON icon = nullptr;
    if (mask) {
        ICONINFO info{};
        info.fIcon = TRUE;
        info.hbmColor = color;
        info.hbmMask = mask;
        icon = CreateIconIndirect(&info);
        DeleteObject(mask);
    }
    DeleteObject(color);
    return icon;
}

GUID tray_guid(std::wstring_view id) {
    // Stable per-device identity for the Windows notification-area preferences.
    uint64_t first = 0xc578062fca58f001ULL;
    uint64_t second = 0x46f396fadf9540a7ULL;
    for (const wchar_t ch : id) {
        first = (first ^ ch) * 0x100000001b3ULL;
        second = (second ^ (ch + 0x10000ULL)) * 0x9e3779b185ebca87ULL;
    }
    GUID guid{
        static_cast<DWORD>(first >> 32),
        static_cast<WORD>(first >> 16),
        static_cast<WORD>((first & 0x0fff) | 0x8000),
        {}
    };
    for (int i = 0; i < 8; ++i) guid.Data4[i] = static_cast<BYTE>(second >> (56 - i * 8));
    guid.Data4[0] = (guid.Data4[0] & 0x3f) | 0x80;
    return guid;
}

std::wstring shorten(std::wstring_view text, size_t limit) {
    if (text.size() <= limit) return std::wstring(text);
    size_t end = limit - 1;
    if (end && IS_HIGH_SURROGATE(text[end - 1])) --end;
    return std::wstring(text.substr(0, end)) + L"…";
}

std::wstring menu_label(std::wstring_view name) {
    std::wstring escaped;
    for (wchar_t ch : shorten(name, 52)) {
        escaped += ch;
        if (ch == L'&') escaped += ch;
    }
    return escaped;
}

struct TrayEntry {
    UINT id;
    GUID guid;
    HICON icon = nullptr;
    std::optional<unsigned> percent;
    std::wstring tooltip;
    bool registered = false;
};

struct ScanMailbox {
    std::mutex lock;
    std::optional<BatteryScan> ready;
};

class TrayApp {
public:
    bool open(HINSTANCE instance) {
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");

        WNDCLASSW cls{};
        cls.lpfnWndProc = &TrayApp::window_proc;
        cls.hInstance = instance;
        cls.lpszClassName = window_class;
        if (!RegisterClassW(&cls)) return false;

        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, window_class, app_name, WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (!hwnd_) return false;

        strip_.open(instance, hwnd_, menu_message);
        start_scan();
        SetTimer(hwnd_, timer_id, refresh_ms, nullptr);
        SetTimer(hwnd_, placement_timer_id, placement_ms, nullptr);
        return true;
    }

private:
    HWND hwnd_ = nullptr;
    UINT taskbar_created_ = 0;
    bool scanning_ = false;
    bool scan_failed_ = false;
    bool menu_open_ = false;
    UINT next_id_ = 1;
    std::shared_ptr<ScanMailbox> mailbox_ = std::make_shared<ScanMailbox>();
    std::vector<BatteryReading> readings_;
    TaskbarStrip strip_;
    std::map<std::wstring, TrayEntry> entries_;

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto* app = static_cast<TrayApp*>(create->lpCreateParams);
            app->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        auto* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return app ? app->on_message(message, wparam, lparam)
                   : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT on_message(UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbar_created_ && message == taskbar_created_) {
            for (auto& [id, entry] : entries_) entry.registered = false;
            update_presentation();
            return 0;
        }

        switch (message) {
        case WM_TIMER:
            if (wparam == timer_id) start_scan();
            if (wparam == placement_timer_id && !readings_.empty()) update_presentation();
            return 0;
        case scan_message:
            finish_scan();
            return 0;
        case menu_message:
            show_menu(0);
            return 0;
        case tray_message: {
            const auto event = LOWORD(lparam);
            const auto id = HIWORD(lparam);
            if (event == WM_LBUTTONUP || event == WM_CONTEXTMENU || event == NIN_SELECT
                || event == NIN_KEYSELECT || (event == WM_RBUTTONUP && !id)) {
                show_menu(id ? id : static_cast<UINT>(wparam));
            }
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            strip_.appearance_changed();
            for (auto& [id, entry] : entries_) {
                if (entry.icon) DestroyIcon(entry.icon);
                entry.icon = nullptr;
            }
            update_presentation();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, timer_id);
            KillTimer(hwnd_, placement_timer_id);
            strip_.close();
            clear_tray();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wparam, lparam);
        }
    }

    void start_scan() {
        if (scanning_) return;
        scanning_ = true;
        try {
            std::thread([hwnd = hwnd_, mailbox = mailbox_]() {
                BatteryScan result;
                try {
                    result = scan_batteries();
                } catch (...) {
                    result.enumeration_failed = true;
                }
                {
                    std::lock_guard guard(mailbox->lock);
                    mailbox->ready.emplace(std::move(result));
                }
                PostMessageW(hwnd, scan_message, 0, 0);
            }).detach();
        } catch (...) {
            scanning_ = false;
            scan_failed_ = true;
        }
        update_presentation();
    }

    void finish_scan() {
        std::optional<BatteryScan> result;
        {
            std::lock_guard guard(mailbox_->lock);
            result.swap(mailbox_->ready);
        }
        if (!result) return;
        scanning_ = false;
        scan_failed_ = result->enumeration_failed;
        readings_ = std::move(result->readings);
        strip_.set_readings(readings_);
        update_presentation();
    }

    NOTIFYICONDATAW notify_data(const TrayEntry& entry) const {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = entry.id;
        data.guidItem = entry.guid;
        data.uFlags = NIF_GUID;
        return data;
    }

    void remove_icon(TrayEntry& entry) {
        if (entry.registered) {
            auto data = notify_data(entry);
            Shell_NotifyIconW(NIM_DELETE, &data);
        }
        if (entry.icon) DestroyIcon(entry.icon);
    }

    UINT claim_id() {
        while (true) {
            const UINT id = next_id_;
            next_id_ = next_id_ == 0xffff ? 1 : next_id_ + 1;
            if (std::none_of(entries_.begin(), entries_.end(), [id](const auto& item) {
                return item.second.id == id;
            })) return id;
        }
    }

    void set_icon(const std::wstring& key, std::optional<unsigned> level, std::wstring tooltip) {
        auto found = entries_.find(key);
        if (found == entries_.end()) {
            found = entries_.emplace(key, TrayEntry{claim_id(), tray_guid(key), nullptr, {}, {}, false}).first;
        }
        TrayEntry& entry = found->second;
        const bool icon_changed = !entry.icon || entry.percent != level;
        if (!icon_changed && entry.tooltip == tooltip && entry.registered) return;

        if (icon_changed) {
            const HICON updated = make_icon(level);
            if (!updated) return;
            if (entry.icon) DestroyIcon(entry.icon);
            entry.icon = updated;
        }
        entry.percent = level;
        entry.tooltip = std::move(tooltip);

        auto data = notify_data(entry);
        data.uFlags |= NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = tray_message;
        data.hIcon = entry.icon;
        const auto tip = shorten(entry.tooltip, std::size(data.szTip) - 1);
        std::copy(tip.begin(), tip.end(), data.szTip);

        if (entry.registered && Shell_NotifyIconW(NIM_MODIFY, &data)) return;
        entry.registered = Shell_NotifyIconW(NIM_ADD, &data);
        if (entry.registered) {
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);
        }
    }

    const wchar_t* empty_status() const {
        if (scanning_) return L"Procurando baterias…";
        if (scan_failed_) return L"Falha ao consultar dispositivos";
        return L"Nenhuma bateria disponível";
    }

    void clear_tray() {
        for (auto& [id, entry] : entries_) remove_icon(entry);
        entries_.clear();
    }

    void update_presentation() {
        if (strip_.sync()) {
            clear_tray();
            return;
        }
        if (readings_.empty()) {
            set_icon(empty_id, std::nullopt, std::wstring(app_name) + L" · " + empty_status());
        }
        for (const auto& reading : readings_) {
            const auto tooltip = std::to_wstring(reading.percent) + L"% · " + shorten(reading.name, 95)
                + (reading.from_windows ? L" · Windows" : L"");
            set_icon(reading.id, reading.percent, tooltip);
        }
        for (auto it = entries_.begin(); it != entries_.end();) {
            const bool needed = readings_.empty() ? it->first == empty_id
                : std::any_of(readings_.begin(), readings_.end(), [&](const BatteryReading& reading) {
                    return reading.id == it->first;
                });
            if (needed) {
                ++it;
            } else {
                remove_icon(it->second);
                it = entries_.erase(it);
            }
        }
    }

    POINT menu_position(UINT id) const {
        for (const auto& [key, entry] : entries_) {
            if (!entry.registered || (id && entry.id != id)) continue;
            NOTIFYICONIDENTIFIER target{};
            target.cbSize = sizeof(target);
            target.hWnd = hwnd_;
            target.uID = entry.id;
            target.guidItem = entry.guid;
            RECT rect{};
            if (SUCCEEDED(Shell_NotifyIconGetRect(&target, &rect))) {
                return {(rect.left + rect.right) / 2, rect.top};
            }
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        return cursor;
    }

    void show_menu(UINT icon_id) {
        if (menu_open_) return;
        const HMENU menu = CreatePopupMenu();
        if (!menu) return;
        menu_open_ = true;

        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, app_name);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (readings_.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, empty_status());
        for (const auto& reading : readings_) {
            const auto label = std::to_wstring(reading.percent) + L"%   " + menu_label(reading.name);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, label.c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (scanning_ ? MF_GRAYED : 0), refresh_command,
                    scanning_ ? L"Atualizando…" : L"&Atualizar agora");
        AppendMenuW(menu, MF_STRING, quit_command, L"&Sair");

        const POINT position = menu_position(icon_id);
        SetForegroundWindow(hwnd_);
        const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON
                                           | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                                           position.x, position.y, 0, hwnd_, nullptr);
        // Allows the menu to dismiss on a click elsewhere.
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        menu_open_ = false;

        if (!choice) {
            for (const auto& [key, entry] : entries_) {
                if (!entry.registered || entry.id != icon_id) continue;
                auto data = notify_data(entry);
                Shell_NotifyIconW(NIM_SETFOCUS, &data);
                break;
            }
        }
        if (choice == refresh_command) start_scan();
        if (choice == quit_command) DestroyWindow(hwnd_);
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const HANDLE single_instance = CreateMutexW(nullptr, FALSE, instance_mutex);
    if (!single_instance) {
        MessageBoxW(nullptr, L"Não foi possível iniciar o aplicativo.", app_name, MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (const HWND window = FindWindowW(window_class, app_name)) {
            DWORD process_id = 0;
            GetWindowThreadProcessId(window, &process_id);
            if (process_id) AllowSetForegroundWindow(process_id);
            PostMessageW(window, menu_message, 0, 0);
        }
        CloseHandle(single_instance);
        return 0;
    }

    TrayApp app;
    if (!app.open(instance)) {
        MessageBoxW(nullptr, L"Não foi possível abrir o ícone na bandeja.",
                    app_name, MB_OK | MB_ICONERROR);
        CloseHandle(single_instance);
        return 1;
    }

    MSG message{};
    BOOL result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CloseHandle(single_instance);
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}
