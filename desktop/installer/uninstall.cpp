#include <windows.h>
#include <msi.h>
#include <shellapi.h>
#include <string>
#include <string_view>

#include <vector>

namespace camdroid::installer::utils {

    std::wstring bundle_path() {

        HKEY root = nullptr;

        if (RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)",
                0,
                KEY_READ | KEY_WOW64_64KEY,

                &root
            ) != ERROR_SUCCESS)
            return {};

        std::wstring result;

        for (DWORD index = 0;; index++) {

            wchar_t name[256]{};
            DWORD length = 256;
            const LSTATUS status = RegEnumKeyExW(root, index, name, &length, nullptr, nullptr, nullptr, nullptr);

            if (status == ERROR_NO_MORE_ITEMS)
                break;

            if (status != ERROR_SUCCESS)
                continue;

            wchar_t codes[4096]{};
            DWORD size = sizeof(codes);

            if (RegGetValueW(root, name, L"BundleUpgradeCode", RRF_RT_REG_MULTI_SZ, nullptr, codes, &size) != ERROR_SUCCESS)
                continue;

            bool matches = false;

            for (const wchar_t* code = codes; *code; code += wcslen(code) + 1)
                if (_wcsicmp(code, L"{D4110760-2522-4197-8F36-6FB64EE26256}") == 0)
                    matches = true;

            if (!matches)
                continue;

            wchar_t path[32768]{};
            size = sizeof(path);

            if (RegGetValueW(root, name, L"BundleCachePath", RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS) {

                result = path;
                break;

            }

        }

        RegCloseKey(root);

        return result;

    }

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR options, int) {

    const bool quiet = std::wstring_view(options) == L"--quiet";
    const auto bundle = camdroid::installer::utils::bundle_path();

    if (!bundle.empty()) {

        const auto launched = reinterpret_cast<INT_PTR>(ShellExecuteW(
            nullptr,
            L"open",
            bundle.c_str(),
            quiet ? L"/uninstall /quiet /norestart" : L"/uninstall",

            nullptr,
            SW_SHOWNORMAL
        ));

        return launched > 32 ? 0 : static_cast<int>(launched);

    }

    wchar_t product[39]{};
    const UINT result = MsiEnumRelatedProductsW(L"{5731CE9C-0416-4C59-AF52-39083C9D6402}", 0, 0, product);

    if (result == ERROR_NO_MORE_ITEMS) {

        if (!quiet)
            MessageBoxW(nullptr, L"CamDroid is not installed.", L"CamDroid", MB_OK | MB_ICONINFORMATION);

        return 0;

    }

    if (result != ERROR_SUCCESS)
        return static_cast<int>(result);

    wchar_t system[MAX_PATH]{};

    if (!GetSystemDirectoryW(system, MAX_PATH))
        return static_cast<int>(GetLastError());

    const std::wstring executable = std::wstring(system) + L"\\msiexec.exe";
    const std::wstring arguments = std::wstring(L"/x ") + product + (quiet ? L" /qn /norestart" : L"");
    const auto launched = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", executable.c_str(), arguments.c_str(), nullptr, SW_SHOWNORMAL)
    );

    // Exit before MSI removes this executable Windows Installer owns the uninstall UI.
    return launched > 32 ? 0 : static_cast<int>(launched);

}