#include <windows.h>
#include <iostream>
#include <string>
#include <vector>

int wmain() {

    wchar_t path[32768]{};

    if (!GetModuleFileNameW(nullptr, path, 32768))
        return 1;

    std::wstring executable(path);
    executable.replace(executable.find_last_of(L'.'), std::wstring::npos, L".exe");
    const wchar_t* tail = GetCommandLineW();

    while (*tail == L' ' || *tail == L'\t')
        tail++;

    if (*tail == L'"') {

        tail++;

        while (*tail && *tail != L'"')
            tail++;

        if (*tail)
            tail++;

    } else {

        while (*tail && *tail != L' ' && *tail != L'\t')
            tail++;

    }

    const std::wstring command = L"\"" + executable + L"\"" + tail;
    std::vector<wchar_t> arguments(command.begin(), command.end());
    arguments.push_back(L'\0');
    STARTUPINFOW startup{sizeof(STARTUPINFOW)};
    startup.dwFlags = STARTF_USESTDHANDLES;

    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};

    if (!CreateProcessW(
            executable.c_str(),
            arguments.data(),
            nullptr,
            nullptr,

            TRUE,
            0,
            nullptr,
            nullptr,
            &startup,

            &process
        )) {

        std::cerr << "Could not start CamDroid. Windows error: " << GetLastError() << '\n';

        return 1;

    }

    CloseHandle(process.hThread);
    // The child handles Ctrl+C keep this launcher alive until its exit code is available.
    SetConsoleCtrlHandler(nullptr, TRUE);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 1;

    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);

    return static_cast<int>(result);

}