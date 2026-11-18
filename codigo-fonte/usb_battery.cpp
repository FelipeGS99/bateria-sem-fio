#include "battery.hpp"
#include "device_handles.hpp"
#include "usb_reports.hpp"

#include <hidsdi.h>
#include <hidpi.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <set>
#include <string_view>

namespace {

using Report = std::span<const uint8_t>;
constexpr DWORD transfer_timeout_ms = 350;
constexpr size_t max_report_size = 256;
constexpr size_t hyperx_report_size = 62;

enum class Receiver { unsupported, lightspeed, logitech_mouse, logitech_keyboard, hyperx };

Receiver receiver_type(const HIDD_ATTRIBUTES& attributes) {
    if (attributes.VendorID == 0x03f0) {
        switch (attributes.ProductID) {
        case 0x05b7:
        case 0x0c9d:
        case 0x0fcc:
            return Receiver::hyperx;
        }
    }
    if (attributes.VendorID == 0x046d) {
        switch (attributes.ProductID) {
        case 0xc539:
        case 0xc53a:
        case 0xc53d:
        case 0xc53f:
        case 0xc541:
        case 0xc545:
        case 0xc547:
        case 0xc54d:
            return Receiver::lightspeed;
        case 0xc094:
            return Receiver::logitech_mouse;
        case 0xc352:
            return Receiver::logitech_keyboard;
        }
    }
    return Receiver::unsupported;
}

bool report_length(size_t size, size_t minimum) {
    return size >= minimum && size <= max_report_size;
}

bool supports_reports(Receiver receiver, const HIDP_CAPS& caps) {
    if (receiver == Receiver::hyperx) {
        return caps.UsagePage == 0xff13 && caps.Usage == 1
            && report_length(caps.InputReportByteLength, hyperx_report_size)
            && (report_length(caps.OutputReportByteLength, hyperx_report_size)
                || report_length(caps.FeatureReportByteLength, hyperx_report_size));
    }
    return receiver != Receiver::unsupported && caps.UsagePage >= 0xff00
        && report_length(caps.InputReportByteLength, 20)
        && report_length(caps.OutputReportByteLength, 20);
}

class HidPort {
public:
    HidPort(HANDLE handle, const HIDP_CAPS& caps)
        : handle_(handle), caps_(caps), ready_{CreateEventW(nullptr, TRUE, FALSE, nullptr)} {}

    bool send(Report request, size_t minimum_size, bool allow_feature = false) {
        std::array<uint8_t, max_report_size> frame{};
        std::copy(request.begin(), request.end(), frame.begin());
        const DWORD output_size = caps_.OutputReportByteLength;
        if (report_length(output_size, minimum_size)
            && transfer(frame.data(), output_size, transfer_timeout_ms, true) == output_size)
            return true;

        // Some Cloud III receivers expose the query as a feature report.
        const DWORD feature_size = caps_.FeatureReportByteLength;
        return allow_feature && report_length(feature_size, minimum_size)
            && HidD_SetFeature(handle_, frame.data(), feature_size);
    }

    Report read_until(ULONGLONG deadline) {
        const auto now = GetTickCount64();
        if (now >= deadline) return {};
        const DWORD length = transfer(reply_.data(), caps_.InputReportByteLength,
                                      static_cast<DWORD>(deadline - now), false);
        return {reply_.data(), length};
    }

private:
    HANDLE handle_;
    HIDP_CAPS caps_;
    DeviceFile ready_;
    std::array<uint8_t, max_report_size> reply_{};

    DWORD transfer(uint8_t* buffer, DWORD size, DWORD timeout_ms, bool output) const {
        if (!ready_.handle || !ResetEvent(ready_.handle)) return 0;
        OVERLAPPED operation{};
        operation.hEvent = ready_.handle;
        DWORD length = 0;
        const BOOL completed = output
            ? WriteFile(handle_, buffer, size, &length, &operation)
            : ReadFile(handle_, buffer, size, &length, &operation);
        if (!completed) {
            if (GetLastError() != ERROR_IO_PENDING) return 0;
            if (WaitForSingleObject(ready_.handle, timeout_ms) != WAIT_OBJECT_0) {
                CancelIoEx(handle_, &operation);
                // Windows still owns the operation and buffer until cancellation completes.
                GetOverlappedResult(handle_, &operation, &length, TRUE);
                return 0;
            }
        }
        return GetOverlappedResult(handle_, &operation, &length, FALSE) ? length : 0;
    }
};

class Hidpp {
public:
    explicit Hidpp(HidPort& port) : port_(port) {}

    std::optional<unsigned> battery(uint8_t slot) {
        const auto unified = feature(slot, 0x1004);
        if (!unified) return std::nullopt;
        if (*unified) {
            const auto capabilities = request(slot, *unified, 0);
            if (capabilities && ((*capabilities)[1] & 0x02)) {
                if (const auto state = request(slot, *unified, 1)) {
                    if (const auto value = usb_reports::percentage((*state)[0])) return value;
                }
            }
        }

        const auto legacy = feature(slot, 0x1000);
        if (!legacy || !*legacy) return std::nullopt;
        const auto state = request(slot, *legacy, 0);
        // In the legacy feature a zero state of charge means unavailable.
        if (!state || (*state)[0] == 0) return std::nullopt;
        return usb_reports::percentage((*state)[0]);
    }

    std::wstring device_name(uint8_t slot) {
        const auto index = feature(slot, 0x0005);
        if (!index || !*index) return {};
        const auto size_reply = request(slot, *index, 0);
        if (!size_reply || (*size_reply)[0] == 0 || (*size_reply)[0] > 64) return {};
        const size_t size = (*size_reply)[0];

        std::string name;
        while (name.size() < size) {
            const auto part = request(slot, *index, 1, static_cast<uint8_t>(name.size()));
            if (!part) return {};
            const auto count = std::min(part->size(), size - name.size());
            name.append(reinterpret_cast<const char*>(part->data()), count);
        }

        std::wstring result(name.size(), L'\0');
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             name.data(), static_cast<int>(name.size()),
                                             result.data(), static_cast<int>(result.size()));
        if (count == 0) return {};
        result.resize(count);
        for (wchar_t& ch : result) {
            if (ch < L' ') ch = L' ';
        }
        const auto start = result.find_first_not_of(L' ');
        if (start == std::wstring::npos) return {};
        return result.substr(start, result.find_last_not_of(L' ') - start + 1);
    }

private:
    HidPort& port_;
    uint8_t software_id_ = 0x0c;

    std::optional<uint8_t> feature(uint8_t slot, uint16_t id) {
        const auto result = request(slot, 0, 0, static_cast<uint8_t>(id >> 8),
                                    static_cast<uint8_t>(id & 0xff));
        if (!result) return std::nullopt;
        return (*result)[0];
    }

    std::optional<std::vector<uint8_t>> request(uint8_t slot, uint8_t feature, uint8_t function,
                                               uint8_t first = 0, uint8_t second = 0) {
        const auto function_id = static_cast<uint8_t>((function << 4) | software_id_);
        software_id_ = software_id_ == 0x0f ? 0x08 : software_id_ + 1;
        const std::array<uint8_t, 6> frame{
            usb_reports::hidpp_long, slot, feature, function_id, first, second
        };
        if (!port_.send(frame, 20)) return std::nullopt;

        const auto deadline = GetTickCount64() + transfer_timeout_ms;
        for (auto report = port_.read_until(deadline); !report.empty(); report = port_.read_until(deadline)) {
            switch (usb_reports::hidpp_reply(report, slot, feature, function_id)) {
            case usb_reports::HidppReply::response: {
                const size_t size = std::min<size_t>(report.size(), report[0] == usb_reports::hidpp_long ? 20u : 7u);
                return std::vector<uint8_t>(report.begin() + 4, report.begin() + size);
            }
            case usb_reports::HidppReply::error:
                return std::nullopt;
            case usb_reports::HidppReply::unrelated:
                break;
            }
        }
        return std::nullopt;
    }
};

std::optional<unsigned> hyperx_battery(HidPort& port) {
    if (!port.send(usb_reports::hyperx_battery_request, hyperx_report_size, true))
        return std::nullopt;
    const auto deadline = GetTickCount64() + transfer_timeout_ms * 2;
    for (auto report = port.read_until(deadline); !report.empty(); report = port.read_until(deadline)) {
        if (const auto value = usb_reports::hyperx_percentage(report)) return value;
    }
    return std::nullopt;
}

void add_logitech(BatteryScan& scan, std::set<std::wstring>& recorded, const std::wstring& id,
                  Receiver receiver, HidPort& port) {
    Hidpp hidpp(port);
    constexpr std::array<uint8_t, 2> wireless_slots{1, 2};
    constexpr std::array<uint8_t, 1> direct_slot{0xff};
    const auto slots = receiver == Receiver::lightspeed
        ? std::span<const uint8_t>(wireless_slots) : std::span<const uint8_t>(direct_slot);
    for (const uint8_t slot : slots) {
        auto key = id + L":" + std::to_wstring(slot);
        if (recorded.contains(key)) continue;
        const auto value = hidpp.battery(slot);
        if (!value) continue;

        auto name = hidpp.device_name(slot);
        if (name.empty() && receiver == Receiver::logitech_mouse) name = L"PRO X SUPERLIGHT";
        if (name.empty() && receiver == Receiver::logitech_keyboard) name = L"PRO X TKL";
        if (name.empty()) name = L"LIGHTSPEED " + std::to_wstring(slot);
        if (!name.starts_with(L"Logitech")) name.insert(0, L"Logitech ");
        recorded.insert(key);
        scan.readings.push_back({std::move(key), std::move(name), *value, false});
    }
}

} // namespace

void append_usb_batteries(BatteryScan& scan) {
    GUID hid_guid{};
    HidD_GetHidGuid(&hid_guid);
    DeviceList list{SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)};
    if (list.handle == INVALID_HANDLE_VALUE) return;

    std::set<std::wstring> recorded;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA device_interface{};
        device_interface.cbSize = sizeof(device_interface);
        if (!SetupDiEnumDeviceInterfaces(list.handle, nullptr, &hid_guid, index, &device_interface))
            break;

        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(list.handle, &device_interface, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::unique_ptr<BYTE[]> storage(new BYTE[size]{});
        auto* details = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.get());
        details->cbSize = sizeof(*details);
        SP_DEVINFO_DATA node{};
        node.cbSize = sizeof(node);
        if (!SetupDiGetDeviceInterfaceDetailW(list.handle, &device_interface, details, size,
                                             nullptr, &node)) continue;

        std::wstring path(details->DevicePath);
        for (auto& ch : path) ch = static_cast<wchar_t>(std::towlower(ch));
        if (path.find(L"hid#vid_046d&") == std::wstring::npos
            && path.find(L"hid#vid_03f0&") == std::wstring::npos) continue;

        constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
        DeviceFile info{CreateFileW(details->DevicePath, 0, sharing, nullptr,
                                    OPEN_EXISTING, 0, nullptr)};
        if (info.handle == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (!HidD_GetAttributes(info.handle, &attributes)) continue;
        const auto receiver = receiver_type(attributes);
        if (receiver == Receiver::unsupported) continue;

        PHIDP_PREPARSED_DATA data = nullptr;
        if (!HidD_GetPreparsedData(info.handle, &data)) continue;
        HIDP_CAPS caps{};
        const auto status = HidP_GetCaps(data, &caps);
        HidD_FreePreparsedData(data);
        if (status != HIDP_STATUS_SUCCESS || !supports_reports(receiver, caps)) continue;

        auto container = container_identifier(list.handle, node);
        const auto id = std::wstring(L"usb:") + (container.empty() ? path : container);
        if (receiver == Receiver::hyperx && recorded.contains(id)) continue;

        DeviceFile file{CreateFileW(details->DevicePath, GENERIC_READ | GENERIC_WRITE, sharing,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
        if (file.handle == INVALID_HANDLE_VALUE) continue;
        HidPort port(file.handle, caps);

        if (receiver == Receiver::hyperx) {
            if (const auto value = hyperx_battery(port)) {
                recorded.insert(id);
                scan.readings.push_back({id, L"HyperX Cloud III Wireless", *value, false});
            }
        } else {
            add_logitech(scan, recorded, id, receiver, port);
        }
    }
}
