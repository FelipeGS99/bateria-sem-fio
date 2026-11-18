#pragma once

#include <windows.h>
#include <setupapi.h>

#include <string>

struct DeviceList {
    HDEVINFO handle;
    ~DeviceList() {
        if (handle != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(handle);
    }
};

struct DeviceFile {
    HANDLE handle;
    ~DeviceFile() {
        if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

std::wstring container_identifier(HDEVINFO list, SP_DEVINFO_DATA& node);
