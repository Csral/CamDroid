#include "arg_parser.h"
#include "cli.h"
#include "preview.h"
#include "virtual_camera.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <QResource>

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <QApplication>
#include <QMessageBox>
#endif

int main(int argc, char* argv[]) {

    Q_INIT_RESOURCE(camdroid_branding);

    #ifdef _WIN32
    // GUI launches never allocate a console. CLI launches use their parent's console or redirected handles.
    const HANDLE original_output = GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE original_error = GetStdHandle(STD_ERROR_HANDLE);
    const DWORD output_type = GetFileType(original_output);
    const DWORD error_type = GetFileType(original_error);

    if (argc > 1 && AttachConsole(ATTACH_PARENT_PROCESS)) {

        FILE* stream = nullptr;

        if (output_type == FILE_TYPE_PIPE || output_type == FILE_TYPE_DISK)
            SetStdHandle(STD_OUTPUT_HANDLE, original_output);
        else
            freopen_s(&stream, "CONOUT$", "w", stdout);

        if (error_type == FILE_TYPE_PIPE || error_type == FILE_TYPE_DISK)
            SetStdHandle(STD_ERROR_HANDLE, original_error);
        else
            freopen_s(&stream, "CONOUT$", "w", stderr);

    }

    #endif

    try {

        const auto configuration = camdroid::argument_parser(argc, argv);

        if (configuration.help) {

            camdroid::show_help();

            return 0;

        }

        if (configuration.install_webcam || configuration.uninstall_webcam)
            return camdroid::virtual_camera::install_source(configuration.uninstall_webcam);

        if (configuration.headless)
            return camdroid::run_cli(configuration);

        return camdroid::run_preview(argc, argv, configuration);

    } catch (const std::exception& error) {

        #ifdef _WIN32
        if (argc == 1) {

            QApplication application(argc, argv);
            QMessageBox::critical(nullptr, "CamDroid", QString::fromUtf8(error.what()));

            return 1;

        }

        #endif
        std::cerr << "CamDroid failure: " << error.what() << '\n';

        return 1;

    }

}