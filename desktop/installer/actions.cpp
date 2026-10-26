#include "camera_install.h"
#include <msi.h>
#include <msiquery.h>

namespace camdroid::installer::utils {

    void log(MSIHANDLE install, const char* text) {

        const MSIHANDLE record = MsiCreateRecord(1);

        if (!record)
            return;

        MsiRecordSetStringA(record, 0, text);
        MsiProcessMessage(install, INSTALLMESSAGE_INFO, record);
        MsiCloseHandle(record);

    }

}

extern "C" __declspec(dllexport) UINT __stdcall PrepareCameraData(MSIHANDLE install) {

    try {

        const auto directory = camdroid::webcam::program_directory(FOLDERID_ProgramData);
        camdroid::webcam::security access(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;FRFX;;;LS)(A;;0x1200ad;;;BU)(A;OICIIO;FA;;;CO)" +
            camdroid::webcam::frame_server_access()
        );

        if (!CreateDirectoryW(directory.c_str(), &access.attributes))
            camdroid::webcam::utils::install_check(GetLastError() == ERROR_ALREADY_EXISTS);

        return ERROR_SUCCESS;

    } catch (const std::exception& error) {

        camdroid::installer::utils::log(install, error.what());

        return ERROR_INSTALL_FAILURE;

    }

}

extern "C" __declspec(dllexport) UINT __stdcall CleanLegacyCamera(MSIHANDLE install) {

    try {

        wchar_t keep[32768]{};
        DWORD length = 32768;
        camdroid::webcam::utils::install_check(MsiGetPropertyW(install, L"CustomActionData", keep, &length) == ERROR_SUCCESS);
        camdroid::webcam::utils::clean_camera_files(camdroid::webcam::program_directory(FOLDERID_ProgramFiles), keep);

    } catch (const std::exception& error) {

        // Cleanup never invalidates an otherwise successful transaction.
        camdroid::installer::utils::log(install, error.what());

    }

    return ERROR_SUCCESS;

}