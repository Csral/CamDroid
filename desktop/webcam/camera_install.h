#pragma once
#include "camera_shared.h"
#include <system_error>

namespace camdroid::webcam::utils {

    inline void install_check(bool succeeded, DWORD code = ERROR_SUCCESS) {

        if (!succeeded) {

            if (!code)
                code = GetLastError();

            if (!code)
                code = ERROR_GEN_FAILURE;

            throw std::system_error(static_cast<int>(code), std::system_category());

        }

    }

    class service_handle {

        private:
        SC_HANDLE value;

        public:
        explicit service_handle(SC_HANDLE handle) : value(handle) {}

        ~service_handle() {
            if (value)
                CloseServiceHandle(value);
        }

        service_handle(const service_handle&) = delete;
        service_handle& operator=(const service_handle&) = delete;

        SC_HANDLE get() const {
            return value;
        }

    };

    inline DWORD service_state(SC_HANDLE service) {

        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        install_check(
            QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)
        );

        return status.dwCurrentState;

    }

    inline void wait_service(SC_HANDLE service, DWORD wanted) {

        const ULONGLONG deadline = GetTickCount64() + 15000;

        while (service_state(service) != wanted) {

            install_check(GetTickCount64() < deadline, ERROR_TIMEOUT);
            Sleep(50);

        }

    }

    inline void refresh_camera_host() {

        service_handle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        install_check(manager.get() != nullptr);
        service_handle service(
            OpenServiceW(manager.get(), L"FrameServer", SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START)
        );

        install_check(service.get() != nullptr);
        auto state = service_state(service.get());

        if (state == SERVICE_STOPPED)
            return;

        if (state == SERVICE_START_PENDING)
            wait_service(service.get(), SERVICE_RUNNING);

        if (state != SERVICE_STOP_PENDING) {

            SERVICE_STATUS status{};

            if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &status))
                install_check(GetLastError() == ERROR_SERVICE_NOT_ACTIVE);

        }

        wait_service(service.get(), SERVICE_STOPPED);

        if (!StartServiceW(service.get(), 0, nullptr))
            install_check(GetLastError() == ERROR_SERVICE_ALREADY_RUNNING);

        wait_service(service.get(), SERVICE_RUNNING);

    }

    inline bool camera_dll_name(const std::wstring& name) {

        if (name == L"camdroid_camera.dll")
            return true;

        constexpr wchar_t prefix[] = L"camdroid_camera-";

        if (!name.starts_with(prefix) || !name.ends_with(L".dll") || name.size() != 58)
            return false;

        GUID id{};

        return SUCCEEDED(CLSIDFromString(name.substr(16, 38).c_str(), &id));

    }

    inline bool clean_camera_files(
        const std::wstring& directory,
        const std::wstring& keep = {},
        BOOL(WINAPI* schedule)(LPCWSTR, LPCWSTR, DWORD) = MoveFileExW
    ) {

        WIN32_FIND_DATAW data{};
        const HANDLE search = FindFirstFileW((directory + LR"(\camdroid_camera*.dll)").c_str(), &data);

        if (search == INVALID_HANDLE_VALUE) {

            const auto code = GetLastError();

            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                return false;

            throw std::system_error(static_cast<int>(code), std::system_category());

        }

        struct search_guard {

            HANDLE handle;

            ~search_guard() {
                FindClose(handle);
            }

        } guard{search};

        bool reboot = false;

        do {

            if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !camera_dll_name(data.cFileName))
                continue;

            const auto file = directory + L"\\" + data.cFileName;

            if (_wcsicmp(file.c_str(), keep.c_str()) == 0)
                continue;

            if (DeleteFileW(file.c_str()))
                continue;

            const DWORD code = GetLastError();

            if (code == ERROR_FILE_NOT_FOUND)
                continue;

            if (code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION && code != ERROR_ACCESS_DENIED)
                throw std::system_error(static_cast<int>(code), std::system_category());

            // A source DLL can remain mapped in Windows Camera Frame Server after an app closes.
            install_check(schedule(file.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT));
            reboot = true;

        } while (FindNextFileW(search, &data));

        install_check(GetLastError() == ERROR_NO_MORE_FILES);

        return reboot;

    }

};