#include "camera_install.h"
#include <iostream>
#include <stdexcept>

namespace camdroid::tests {

    std::wstring deferred_file;
    bool deny_schedule = false;

    BOOL WINAPI defer(LPCWSTR from, LPCWSTR to, DWORD flags) {

        if (to || flags != MOVEFILE_DELAY_UNTIL_REBOOT)
            return FALSE;

        deferred_file = from;

        if (deny_schedule) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }

        return TRUE;

    }

    void require(bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    }

    void create(const std::wstring& path) {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, 0, nullptr);
        require(file != INVALID_HANDLE_VALUE, "Cannot create installer test file.");
        CloseHandle(file);
    }

};

int main() {

    using namespace camdroid;
    wchar_t temporary[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temporary);
    const auto directory = std::wstring(temporary) + L"CamDroid-install-test-" + webcam::new_identifier();
    const auto old = directory + LR"(\camdroid_camera.dll)";

    const auto current = directory + L"\\camdroid_camera-" + webcam::new_identifier() + L".dll";
    const auto other = directory + LR"(\camdroid_camera-unrelated.dll)";
    HANDLE locked = INVALID_HANDLE_VALUE;
    int result = 0;

    try {

        tests::require(CreateDirectoryW(directory.c_str(), nullptr), "Cannot create test directory.");
        tests::create(old);
        tests::create(current);
        tests::require(
            webcam::utils::camera_dll_name(current.substr(current.find_last_of(L"\\") + 1)),

            "Versioned DLL name was not recognized."
        );
        tests::create(other);
        locked = CreateFileW(old.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        tests::require(locked != INVALID_HANDLE_VALUE, "Cannot simulate an in-use DLL.");

        tests::require(webcam::utils::clean_camera_files(directory, current, tests::defer), "In-use DLL was not deferred.");
        tests::require(tests::deferred_file == old, "Cleanup scheduled the wrong file.");
        tests::require(
            GetFileAttributesW(current.c_str()) != INVALID_FILE_ATTRIBUTES,

            "Cleanup removed the installed version."
        );
        tests::require(GetFileAttributesW(other.c_str()) != INVALID_FILE_ATTRIBUTES, "Cleanup touched an unrelated file.");
        tests::deny_schedule = true;
        bool failed = false;

        try {

            webcam::utils::clean_camera_files(directory, current, tests::defer);

        } catch (const std::system_error& error) {

            failed = error.code().value() == ERROR_ACCESS_DENIED;

        }

        tests::require(failed, "Cleanup hid the Windows error.");
        CloseHandle(locked);
        locked = INVALID_HANDLE_VALUE;
        tests::deny_schedule = false;

        tests::require(
            !webcam::utils::clean_camera_files(directory, current, tests::defer),
            "Unlocked file should delete immediately."
        );
        tests::require(GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES, "Old DLL remains after cleanup.");
        std::cout << "Versioned webcam cleanup, in-use DLL deferral and error propagation passed.\n";

    } catch (const std::exception& error) {

        std::cerr << error.what() << '\n';
        result = 1;

    }

    if (locked != INVALID_HANDLE_VALUE)
        CloseHandle(locked);

    DeleteFileW(old.c_str());
    DeleteFileW(current.c_str());
    DeleteFileW(other.c_str());
    RemoveDirectoryW(directory.c_str());

    return result;

}