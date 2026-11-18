#include "battery.hpp"
#include "device_handles.hpp"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <bluetoothleapis.h>
#include <initguid.h>
#include <devpkey.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <optional>
#include <string_view>

namespace {

// Some Bluetooth drivers, including Hands-Free profiles, publish the level here.
constexpr DEVPROPKEY battery_key{
    {0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2
};
constexpr GUID battery_service{
    0x180f, 0, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}
};
constexpr GUID battery_level{
    0x2a19, 0, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}
};
constexpr GUID no_container{};
constexpr GUID system_container{
    0, 0, 0, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}
};

struct DeviceIdentity {
    std::wstring container;
    std::wstring address;
    std::wstring instance;
};

struct Candidate {
    std::wstring name;
    int name_priority = 0;
    std::optional<unsigned> windows_level;
    std::optional<unsigned> gatt_level;
};

template <typename T>
T property_value(const std::array<BYTE, 8>& bytes) {
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

std::optional<unsigned> reported_level(HDEVINFO list, SP_DEVINFO_DATA& node) {
    std::array<BYTE, 8> bytes{};
    DEVPROPTYPE type = 0;
    DWORD size = 0;
    if (!SetupDiGetDevicePropertyW(list, &node, &battery_key, &type,
                                   bytes.data(), static_cast<DWORD>(bytes.size()), &size, 0)) {
        return std::nullopt;
    }

    // Signed negative values also fail the range check after conversion.
    ULONGLONG level = 101;
    switch (type) {
    case DEVPROP_TYPE_BYTE:
    case DEVPROP_TYPE_SBYTE:
        if (size == 1) level = bytes[0];
        break;
    case DEVPROP_TYPE_UINT16:
    case DEVPROP_TYPE_INT16:
        if (size == 2) level = property_value<USHORT>(bytes);
        break;
    case DEVPROP_TYPE_UINT32:
    case DEVPROP_TYPE_INT32:
        if (size == 4) level = property_value<ULONG>(bytes);
        break;
    case DEVPROP_TYPE_UINT64:
    case DEVPROP_TYPE_INT64:
        if (size == 8) level = property_value<ULONGLONG>(bytes);
        break;
    }
    if (level > 100) return std::nullopt;
    return static_cast<unsigned>(level);
}

std::wstring device_text(HDEVINFO list, SP_DEVINFO_DATA& node, const DEVPROPKEY& key) {
    std::array<wchar_t, 512> buffer{};
    DEVPROPTYPE type = 0;
    if (!SetupDiGetDevicePropertyW(list, &node, &key, &type,
                                   reinterpret_cast<BYTE*>(buffer.data()),
                                   static_cast<DWORD>(sizeof(buffer)), nullptr, 0)
        || type != DEVPROP_TYPE_STRING) {
        return {};
    }
    buffer.back() = L'\0';
    std::wstring name(buffer.data());
    std::replace_if(name.begin(), name.end(), [](wchar_t ch) { return ch < L' '; }, L' ');
    const auto first = name.find_first_not_of(L' ');
    if (first == std::wstring::npos) return {};
    return name.substr(first, name.find_last_not_of(L' ') - first + 1);
}

std::wstring instance_id(DEVINST node) {
    std::array<wchar_t, MAX_DEVICE_ID_LEN> value{};
    if (CM_Get_Device_IDW(node, value.data(), static_cast<ULONG>(value.size()), 0) != CR_SUCCESS)
        return {};
    std::wstring id(value.data());
    for (wchar_t& ch : id) {
        if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
    }
    return id;
}

bool hex_digit(wchar_t ch) {
    return (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F');
}

std::wstring address_in(std::wstring_view id) {
    const auto marker = id.find(L"DEV_");
    if (marker == std::wstring_view::npos || marker + 16 > id.size()) return {};
    const auto address = id.substr(marker + 4, 12);
    if (!std::all_of(address.begin(), address.end(), hex_digit)
        || (marker + 16 < id.size() && hex_digit(id[marker + 16]))) return {};
    return std::wstring(address);
}

bool bluetooth_node(std::wstring_view id) {
    return id.starts_with(L"BTHENUM\\") || id.starts_with(L"BTHLE\\")
        || id.starts_with(L"BTHLEDEVICE\\") || id.starts_with(L"BTHHFENUM\\")
        || id.starts_with(L"BTHLEENUM\\") || id.starts_with(L"BTHHFPAUDIO\\")
        || id.starts_with(L"BTHA2DP\\") || id.starts_with(L"BTHAVRCP\\");
}

int name_priority(std::wstring_view id) {
    if (id.starts_with(L"BTHENUM\\DEV_") || id.starts_with(L"BTHLE\\DEV_")) return 3;
    return 1;
}

DeviceIdentity identity_for(HDEVINFO list, SP_DEVINFO_DATA& node, std::wstring id) {
    DeviceIdentity identity;
    identity.instance = std::move(id);
    identity.address = address_in(identity.instance);

    identity.container = container_identifier(list, node);

    // A service or audio profile may only carry the address on an ancestor.
    DEVINST parent = node.DevInst;
    for (int depth = 0; identity.address.empty() && depth < 8; ++depth) {
        DEVINST next{};
        if (CM_Get_Parent(&next, parent, 0) != CR_SUCCESS) break;
        parent = next;
        auto parent_id = instance_id(parent);
        if (bluetooth_node(parent_id)) identity.address = address_in(parent_id);
    }
    return identity;
}

using Containers = std::map<std::wstring, std::wstring>;

std::wstring grouping_key(const DeviceIdentity& identity, const Containers& containers) {
    if (!identity.address.empty()) {
        const auto found = containers.find(identity.address);
        if (found != containers.end()) return found->second;
    }
    if (!identity.container.empty()) return identity.container;
    if (!identity.address.empty()) return L"bt:" + identity.address;
    return identity.instance;
}

void keep_lowest(std::optional<unsigned>& value, unsigned level) {
    if (!value || level < *value) value = level;
}

void pick_name(Candidate& candidate, std::wstring name, int priority) {
    if (priority > candidate.name_priority && !name.empty()) {
        candidate.name = std::move(name);
        candidate.name_priority = priority;
    }
}

bool is_battery_level(const BTH_LE_UUID& uuid) {
    return uuid.IsShortUuid ? uuid.Value.ShortUuid == 0x2a19
                            : IsEqualGUID(uuid.Value.LongUuid, battery_level);
}

std::optional<unsigned> read_gatt_level(const wchar_t* path) {
    DeviceFile device{CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr)};
    if (device.handle == INVALID_HANDLE_VALUE) return std::nullopt;

    USHORT count = 0;
    if (BluetoothGATTGetCharacteristics(device.handle, nullptr, 0, nullptr, &count,
                                         BLUETOOTH_GATT_FLAG_NONE)
            != HRESULT_FROM_WIN32(ERROR_MORE_DATA)
        || !count) return std::nullopt;

    std::vector<BTH_LE_GATT_CHARACTERISTIC> characteristics(count);
    USHORT returned = 0;
    if (FAILED(BluetoothGATTGetCharacteristics(device.handle, nullptr, count,
                                                 characteristics.data(), &returned,
                                                 BLUETOOTH_GATT_FLAG_NONE))
        || returned > count) return std::nullopt;

    std::optional<unsigned> level;
    for (USHORT i = 0; i < returned; ++i) {
        auto& characteristic = characteristics[i];
        if (!characteristic.IsReadable || !is_battery_level(characteristic.CharacteristicUuid))
            continue;

        // The characteristic is one byte, so the structure has room for the response.
        BTH_LE_GATT_CHARACTERISTIC_VALUE value{};
        USHORT needed = 0;
        if (SUCCEEDED(BluetoothGATTGetCharacteristicValue(device.handle, &characteristic,
                                                           sizeof(value), &value, &needed,
                                                           BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE))
            && value.DataSize == 1 && value.Data[0] <= 100) {
            keep_lowest(level, value.Data[0]);
        }
    }
    return level;
}

struct BluetoothNode {
    DeviceIdentity identity;
    std::wstring name;
    int priority;
    std::optional<unsigned> level;
};

} // namespace

std::wstring container_identifier(HDEVINFO list, SP_DEVINFO_DATA& node) {
    GUID container{};
    DWORD size = 0;
    DEVPROPTYPE type = 0;
    if (SetupDiGetDevicePropertyW(list, &node, &DEVPKEY_Device_ContainerId, &type,
                                  reinterpret_cast<BYTE*>(&container), sizeof(container), &size, 0)
        && type == DEVPROP_TYPE_GUID && size == sizeof(container)
        && !IsEqualGUID(container, no_container) && !IsEqualGUID(container, system_container)) {
        wchar_t guid[39]{};
        if (StringFromGUID2(container, guid, static_cast<int>(std::size(guid)))) {
            return guid;
        }
    }
    return {};
}

BatteryScan scan_batteries() {
    BatteryScan scan;
    DeviceList nodes{SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES)};
    scan.enumeration_failed = nodes.handle == INVALID_HANDLE_VALUE;
    std::map<std::wstring, Candidate> candidates;
    Containers containers;

    if (!scan.enumeration_failed) {
        std::vector<BluetoothNode> bluetooth_nodes;
        for (DWORD index = 0;; ++index) {
            SP_DEVINFO_DATA node{};
            node.cbSize = sizeof(node);
            if (!SetupDiEnumDeviceInfo(nodes.handle, index, &node)) {
                scan.enumeration_failed = GetLastError() != ERROR_NO_MORE_ITEMS;
                break;
            }
            std::wstring id = instance_id(node.DevInst);
            if (!bluetooth_node(id)) continue;

            const int priority = name_priority(id);
            DeviceIdentity identity = identity_for(nodes.handle, node, std::move(id));
            if (!identity.container.empty() && !identity.address.empty()) {
                if (priority == 3) containers.insert_or_assign(identity.address, identity.container);
                else containers.try_emplace(identity.address, identity.container);
            }
            auto name = device_text(nodes.handle, node, DEVPKEY_NAME);
            bluetooth_nodes.push_back({std::move(identity), std::move(name), priority,
                                       reported_level(nodes.handle, node)});
        }
        for (auto& node : bluetooth_nodes) {
            Candidate& candidate = candidates[grouping_key(node.identity, containers)];
            pick_name(candidate, std::move(node.name), node.priority);
            if (node.level) keep_lowest(candidate.windows_level, *node.level);
        }
    }

    // Windows registers a device interface for the Battery Service of paired LE devices.
    DeviceList services{SetupDiGetClassDevsW(&battery_service, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)};
    if (services.handle != INVALID_HANDLE_VALUE) {
        for (DWORD index = 0;; ++index) {
            SP_DEVICE_INTERFACE_DATA service_interface{};
            service_interface.cbSize = sizeof(service_interface);
            if (!SetupDiEnumDeviceInterfaces(services.handle, nullptr, &battery_service, index,
                                             &service_interface)) break;

            DWORD size = 0;
            SetupDiGetDeviceInterfaceDetailW(services.handle, &service_interface, nullptr, 0, &size, nullptr);
            if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;

            std::unique_ptr<BYTE[]> storage(new BYTE[size]{});
            auto* details = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.get());
            details->cbSize = sizeof(*details);
            SP_DEVINFO_DATA node{};
            node.cbSize = sizeof(node);
            if (!SetupDiGetDeviceInterfaceDetailW(services.handle, &service_interface, details, size,
                                                 nullptr, &node)) continue;

            auto identity = identity_for(services.handle, node, instance_id(node.DevInst));
            if (identity.container.empty() && identity.address.empty() && identity.instance.empty())
                identity.instance = details->DevicePath;

            Candidate& candidate = candidates[grouping_key(identity, containers)];
            if (candidate.windows_level) continue;

            if (const auto level = read_gatt_level(details->DevicePath)) {
                keep_lowest(candidate.gatt_level, *level);
                if (candidate.name.empty()) {
                    pick_name(candidate, device_text(services.handle, node, DEVPKEY_NAME), 1);
                }
            }
        }
    }

    for (auto& [id, candidate] : candidates) {
        const auto level = candidate.windows_level ? candidate.windows_level : candidate.gatt_level;
        if (!level) continue;
        if (candidate.name.empty()) candidate.name = L"Dispositivo Bluetooth";
        scan.readings.push_back({id, std::move(candidate.name), *level,
                                 candidate.windows_level.has_value()});
    }
    append_usb_batteries(scan);
    std::sort(scan.readings.begin(), scan.readings.end(), [](const auto& left, const auto& right) {
        const int names = CompareStringOrdinal(left.name.c_str(), -1, right.name.c_str(), -1, TRUE);
        if (names == CSTR_EQUAL || names == 0) return left.id < right.id;
        return names == CSTR_LESS_THAN;
    });
    return scan;
}
