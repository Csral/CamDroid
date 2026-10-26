#include "virtual_camera.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <stdexcept>
#include <vector>
#include <algorithm>
#include <utility>
#include <cstdlib>

#include <cmath>
#include <cstddef>
#include <system_error>

#ifdef _WIN32
#include "camera_shared.h"
#include "camera_install.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>

#include <wrl/client.h>
#include <shellapi.h>
#include <cstring>
extern "C" {
#include <libavutil/frame.h>

#include <libavutil/dict.h>
#include <libswscale/swscale.h>
}
#endif

namespace camdroid {

    #ifdef _WIN32
    namespace utils {

        template <int components>
        static void orient_plane(
            const std::uint8_t* source,
            int width,
            int height,
            int source_stride,

            int rotation,
            bool mirrored,
            std::uint8_t* destination
        ) {

            if (rotation == 0 && !mirrored) {

                for (int row = 0; row < height; ++row)
                    std::memcpy(
                        destination + static_cast<std::size_t>(row) * width * components,
                        source + static_cast<std::ptrdiff_t>(row) * source_stride,
                        width * components
                    );

                return;

            }

            const int output_width = rotation == 90 || rotation == 270 ? height : width;
            const int output_height = rotation == 90 || rotation == 270 ? width : height;
            const std::ptrdiff_t step = (mirrored ? -1 : 1) * (rotation == 90    ? -source_stride
                                                               : rotation == 270 ? source_stride
                                                               : rotation == 180 ? -components
                                                                                 : components);

            // Tiles retain cache locality when portrait output transposes source rows.
            for (int top = 0; top < output_height; top += 32) {

                for (int left = 0; left < output_width; left += 32) {

                    const int bottom = std::min(top + 32, output_height);
                    const int right = std::min(left + 32, output_width);

                    for (int y = top; y < bottom; ++y) {

                        const int column = mirrored ? output_width - 1 - left : left;
                        int sx = column, sy = y;

                        if (rotation == 90) {

                            sx = y;
                            sy = height - 1 - column;

                        } else if (rotation == 180) {

                            sx = width - 1 - column;
                            sy = height - 1 - y;

                        } else if (rotation == 270) {

                            sx = width - 1 - y;
                            sy = column;

                        }

                        std::ptrdiff_t input = static_cast<std::ptrdiff_t>(sy) * source_stride + sx * components;
                        auto output = (static_cast<std::size_t>(y) * output_width + left) * components;

                        for (int x = left; x < right; ++x) {

                            destination[output] = source[input];

                            if constexpr (components == 2)
                                destination[output + 1] = source[input + 1];

                            input += step;
                            output += components;

                        }

                    }

                }

            }

        }

        static std::wstring executable_path() {

            std::wstring path(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));

            if (!length || length >= path.size())
                throw std::runtime_error("Cannot locate CamDroid executable.");

            path.resize(length);

            return path;

        }

        static std::wstring executable_directory() {
            const auto path = executable_path();

            return path.substr(0, path.find_last_of(L"\\/"));
        }

        static const char* install_stage(unsigned stage) {

            switch (stage) {

                case 1:
                    return "Administrator access";
                case 2:
                    return "Create installation directory";

                case 3:
                    return "Copy webcam DLL";
                case 4:
                    return "Create webcam data directory";

                case 5:
                    return "Load webcam DLL";
                case 6:
                    return "Register webcam source";

                case 7:
                    return "Unregister webcam source";
                case 8:
                    return "Remove webcam files";

                case 9:
                    return "Refresh Windows Camera Frame Server";
                default:
                    return "Webcam installer";

            }

        }

        static std::string elevated_install(bool remove) {

            const auto path = executable_path();
            SHELLEXECUTEINFOW request{sizeof(request)};
            request.fMask = SEE_MASK_NOCLOSEPROCESS;
            request.lpVerb = L"runas";
            request.lpFile = path.c_str();

            request.lpParameters = remove ? L"--uninstall-webcam" : L"--install-webcam";
            request.nShow = SW_HIDE;

            if (!ShellExecuteExW(&request)) {

                const DWORD code = GetLastError();

                if (code == ERROR_CANCELLED)
                    throw std::runtime_error("Webcam installation was cancelled.");

                throw std::system_error(static_cast<int>(code), std::system_category(), "Start webcam installer");

            }

            WaitForSingleObject(request.hProcess, INFINITE);
            DWORD result = 1;
            GetExitCodeProcess(request.hProcess, &result);
            CloseHandle(request.hProcess);

            if (result == ERROR_SUCCESS_REBOOT_REQUIRED)
                return remove
                           ? "Webcam uninstalled. Windows will delete its in-use files at the next restart."
                           : "Webcam updated. Reopen camera apps to use it; old files will be deleted at the next restart.";

            if (result != 0)
                throw std::system_error(
                    static_cast<int>(result & 0xffff),
                    std::system_category(),
                    install_stage(result >> 16)
                );

            return remove ? "Webcam support uninstalled."
                          : "Webcam support installed. Reopen camera apps to use the updated source.";

        }

    };

    struct virtual_camera::implementation {

        std::mutex control;
        std::mutex output;
        std::condition_variable changed;
        std::atomic<int> incoming_fps{30};
        std::atomic<bool> allow_outputs{true};

        std::atomic<bool> enabled{false};
        std::atomic<bool> working{false};
        int command = 0;
        std::string failure;
        std::string installation_notice;

        webcam::frame_channel channel;
        Microsoft::WRL::ComPtr<IMFVirtualCamera> camera;
        HMODULE library = nullptr;
        SwsContext* output_scaler = nullptr;
        SwsContext* input_scaler = nullptr;

        std::vector<std::uint8_t> decoded_nv12;
        std::vector<std::uint8_t> nv12 = std::vector<std::uint8_t>(webcam::image_size);
        std::jthread worker;

        implementation()
            : worker([this](std::stop_token token) {
                  run(token);
              }) {}

        ~implementation() {

            worker.request_stop();
            changed.notify_all();
            worker.join();
            sws_freeContext(output_scaler);
            sws_freeContext(input_scaler);

            if (library)
                FreeLibrary(library);

        }

        void request(int value) {

            std::lock_guard lock(control);

            if (working)
                return;

            failure.clear();
            working = true;
            command = value;
            changed.notify_all();

        }

        void stop() noexcept {

            enabled = false;

            if (camera) {

                camera->Stop();
                camera->Remove();
                camera->Shutdown();
                camera.Reset();

            }

            std::lock_guard lock(output);
            channel.write(nullptr);
            channel.close();

        }

        void start() {

            if (camera)
                return;

            HKEY key = nullptr;
            const auto registration =
                std::wstring(LR"(Software\Classes\CLSID\)") + webcam::source_clsid + LR"(\InprocServer32)";

            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, registration.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
                throw std::runtime_error("Install webcam support before enabling the camera.");

            RegCloseKey(key);

            if (!library)
                library = LoadLibraryW(L"mfsensorgroup.dll");

            using create_camera = HRESULT(WINAPI*)(
                MFVirtualCameraType,
                MFVirtualCameraLifetime,
                MFVirtualCameraAccess,
                LPCWSTR,

                LPCWSTR,
                const GUID*,
                ULONG,
                IMFVirtualCamera**
            );

            const auto create =
                library ? reinterpret_cast<create_camera>(GetProcAddress(library, "MFCreateVirtualCamera")) : nullptr;

            if (!create)
                throw std::runtime_error("The virtual webcam requires Windows 11 or newer.");

            {

                std::lock_guard lock(output);
                channel.create();
                channel.allow_app_outputs(allow_outputs);

            }

            webcam::check(
                create(
                    MFVirtualCameraType_SoftwareCameraSource,
                    MFVirtualCameraLifetime_Session,
                    MFVirtualCameraAccess_CurrentUser,

                    L"CamDroid",
                    webcam::source_clsid,
                    nullptr,
                    0,

                    &camera
                ),
                "Cannot create virtual camera"
            );

            webcam::check(
                camera->SetString(webcam::ipc_attribute, channel.path.c_str()),
                "Cannot configure camera frame channel"
            );
            const HRESULT started = camera->Start(nullptr);

            webcam::check(
                started,
                started == E_ACCESSDENIED ? "Windows denied camera access; check camera privacy permissions"
                                          : "Cannot start virtual camera"
            );
            enabled = true;

        }

        void run(std::stop_token token) {

            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const HRESULT foundation = SUCCEEDED(initialized) ? MFStartup(MF_VERSION, MFSTARTUP_FULL) : initialized;

            while (!token.stop_requested()) {

                int action;

                {

                    std::unique_lock lock(control);

                    changed.wait(lock, [&] {
                        return command || token.stop_requested();
                    });

                    if (token.stop_requested())
                        break;

                    action = std::exchange(command, 0);

                }

                try {

                    webcam::check(foundation, "Cannot initialize Windows camera services");

                    if (action == 1)
                        start();
                    else if (action == 2)
                        stop();

                    else {

                        stop();
                        const auto message = utils::elevated_install(action == 4);
                        CoFreeUnusedLibrariesEx(0, 0);
                        std::lock_guard lock(control);
                        installation_notice = message;

                    }

                } catch (const std::exception& error) {

                    stop();
                    std::lock_guard lock(control);
                    failure = error.what();

                }

                {

                    std::lock_guard lock(control);
                    working = command != 0;

                }

            }

            stop();

            if (SUCCEEDED(foundation))
                MFShutdown();

            if (SUCCEEDED(initialized))
                CoUninitialize();

        }

        void fail(const char* message) {

            enabled = false;
            std::lock_guard lock(control);
            failure = message;
            command = 2;
            working = true;
            changed.notify_all();

        }

        void publish(const std::uint8_t* data, int w, int h, int stride) {

            if (!enabled || !data || w < 1 || h < 1 || w > 16384 || h > 16384 || stride < w * 4)
                return;

            std::lock_guard lock(output);

            if (!enabled)
                return;

            channel.allow_app_outputs(allow_outputs);
            const double ratio =
                std::min({1.0, 3840.0 / w, 3840.0 / h, std::sqrt((3840.0 * 2160) / (static_cast<double>(w) * h))});
            const int fitted_w = std::max(2, static_cast<int>(w * ratio) & ~1);
            const int fitted_h = std::max(2, static_cast<int>(h * ratio) & ~1);

            nv12.resize(static_cast<std::size_t>(fitted_w) * fitted_h * 3 / 2);
            output_scaler = sws_getCachedContext(
                output_scaler,
                w,
                h,

                AV_PIX_FMT_RGBA,
                fitted_w,
                fitted_h,
                AV_PIX_FMT_NV12,
                SWS_BILINEAR,

                nullptr,
                nullptr,
                nullptr
            );

            if (!output_scaler)
                throw std::runtime_error("Cannot convert webcam output.");

            const auto* coefficients = sws_getCoefficients(SWS_CS_ITU709);

            if (sws_setColorspaceDetails(output_scaler, coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16) < 0)
                throw std::runtime_error("Cannot configure webcam colorspace.");

            const std::uint8_t* source[]{data, nullptr, nullptr, nullptr};
            int source_stride[]{stride, 0, 0, 0};
            std::uint8_t* destination[]{nv12.data(), nv12.data() + fitted_w * fitted_h, nullptr, nullptr};
            int destination_stride[]{fitted_w, fitted_w, 0, 0};

            if (sws_scale(output_scaler, source, source_stride, 0, h, destination, destination_stride) != fitted_h)
                throw std::runtime_error("Incomplete webcam conversion.");

            channel.write(nv12.data(), fitted_w, fitted_h, incoming_fps);

        }

    };

    #else
    struct virtual_camera::implementation {

        bool working = false;
        std::string failure;

    };

    #endif

    virtual_camera::virtual_camera() : state(std::make_unique<implementation>()) {}

    virtual_camera::~virtual_camera() = default;

    void virtual_camera::enable(bool enabled) {

        #ifdef _WIN32
        state->request(enabled ? 1 : 2);
        #else
        state->failure = "The virtual webcam backend currently supports Windows 11 only.";
        #endif

    }

    void virtual_camera::install(bool remove) {

        #ifdef _WIN32
        state->request(remove ? 4 : 3);
        #else
        state->failure = "The virtual webcam backend currently supports Windows 11 only.";
        #endif

    }

    bool virtual_camera::active() const {

        #ifdef _WIN32
        return state->enabled;
        #else
        return false;
        #endif

    }

    bool virtual_camera::pending() const {
        return state->working;
    }

    std::string virtual_camera::error() const {

        #ifdef _WIN32
        std::lock_guard lock(state->control);
        #endif
        return state->failure;

    }

    void virtual_camera::allow_app_outputs(bool allow) {

        #ifdef _WIN32
        std::lock_guard lock(state->output);
        state->allow_outputs = allow;
        state->channel.allow_app_outputs(allow);
        #endif

    }

    bool virtual_camera::app_outputs_allowed() const {

        #ifdef _WIN32
        return state->allow_outputs;
        #else
        return false;
        #endif

    }

    void virtual_camera::set_settings(protocol::video_settings settings) {

        #ifdef _WIN32
        std::lock_guard lock(state->output);

        if (settings.fps)
            state->incoming_fps = settings.fps;

        state->channel.allow_app_outputs(state->allow_outputs);
        #endif

    }

    std::optional<protocol::video_format> virtual_camera::take_request() {

        #ifdef _WIN32
        std::lock_guard lock(state->output);

        return state->channel.take_request();
        #else
        return {};
        #endif

    }

    void virtual_camera::clear() {

        #ifdef _WIN32
        std::lock_guard lock(state->output);
        state->channel.write(nullptr);
        #endif

    }

    void virtual_camera::publish_rgba(const std::uint8_t* data, int width, int height, int stride) {

        #ifdef _WIN32
        try {

            state->publish(data, width, height, stride);

        } catch (const std::exception& error) {
            state->fail(error.what());
        }

        #endif

    }

    void virtual_camera::publish_frame(const AVFrame& frame, int rotation, bool mirrored) {

        #ifdef _WIN32
        if (!active() || frame.width < 2 || frame.height < 2 || frame.width > 16384 || frame.height > 16384)
            return;

        try {

            std::lock_guard lock(state->output);

            if (!state->enabled)
                return;

            const double ratio = std::min(
                {1.0,
                 3840.0 / frame.width,
                 3840.0 / frame.height,
                 std::sqrt((3840.0 * 2160) / (static_cast<double>(frame.width) * frame.height))}

            );

            const int width = std::max(2, static_cast<int>(frame.width * ratio) & ~1);
            const int height = std::max(2, static_cast<int>(frame.height * ratio) & ~1);
            const auto pixels = static_cast<std::size_t>(width) * height;

            int cs = SWS_CS_ITU601;

            switch (frame.colorspace) {

                case AVCOL_SPC_BT709:
                    cs = SWS_CS_ITU709;
                    break;
                    
                case AVCOL_SPC_BT2020_NCL:
                case AVCOL_SPC_BT2020_CL:
                    cs = SWS_CS_BT2020;
                    break;
                
                case AVCOL_SPC_UNSPECIFIED:
                    cs = frame.height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
                    break;
                
                default:
                    break;

            }

            const auto* orientation = av_dict_get(frame.metadata, "camdroid.rotation", nullptr, 0);
            const auto* mirror = av_dict_get(frame.metadata, "camdroid.mirrored", nullptr, 0);
            rotation = rotation >= 0 ? rotation : orientation ? std::atoi(orientation->value) : 0;
            mirrored = mirrored != (mirror && mirror->value[0] == '1');

            if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
                throw std::runtime_error("Invalid webcam orientation.");

            state->channel.allow_app_outputs(state->allow_outputs);

            const bool direct = frame.format == AV_PIX_FMT_NV12 && frame.width == width && frame.height == height &&
                                frame.color_range != AVCOL_RANGE_JPEG && cs == SWS_CS_ITU709;

            if (direct) {

                if (!frame.data[0] || !frame.data[1])
                    throw std::runtime_error("Missing NV12 webcam planes.");

                if (rotation == 0 && !mirrored && frame.linesize[0] == width && frame.linesize[1] == width &&
                    frame.data[1] == frame.data[0] + pixels) {

                    state->channel.write(frame.data[0], width, height, state->incoming_fps);
                    return;

                }

                state->nv12.resize(pixels * 3 / 2);
                utils::orient_plane<
                    1>(frame.data[0], width, height, frame.linesize[0], rotation, mirrored, state->nv12.data());
                utils::orient_plane<2>(
                    frame.data[1],

                    width / 2,
                    height / 2,
                    frame.linesize[1],
                    rotation,
                    mirrored,

                    state->nv12.data() + pixels
                );
                const bool swapped = rotation == 90 || rotation == 270;
                state->channel
                    .write(state->nv12.data(), swapped ? height : width, swapped ? width : height, state->incoming_fps);
                return;

            }

            state->decoded_nv12.resize(pixels * 3 / 2);
            state->input_scaler = sws_getCachedContext(
                state->input_scaler,
                frame.width,
                frame.height,

                static_cast<AVPixelFormat>(frame.format),
                width,
                height,
                AV_PIX_FMT_NV12,
                SWS_BILINEAR,

                nullptr,
                nullptr,
                nullptr
            );

            if (!state->input_scaler)
                throw std::runtime_error("Cannot convert decoded video to NV12.");

            const auto* source_coefficients = sws_getCoefficients(cs);
            const auto* output_coefficients = sws_getCoefficients(SWS_CS_ITU709);

            if (sws_setColorspaceDetails(
                    state->input_scaler,
                    source_coefficients,
                    frame.color_range == AVCOL_RANGE_JPEG,
                    output_coefficients,

                    0,
                    0,
                    1 << 16,
                    1 << 16
                ) < 0)
                throw std::runtime_error("Cannot configure webcam colorspace.");

            std::uint8_t* planes[]{state->decoded_nv12.data(), state->decoded_nv12.data() + pixels, nullptr, nullptr};
            int strides[]{width, width, 0, 0};

            if (sws_scale(state->input_scaler, frame.data, frame.linesize, 0, frame.height, planes, strides) != height)
                throw std::runtime_error("Incomplete NV12 webcam conversion.");

            if (rotation == 0 && !mirrored) {
                state->channel.write(state->decoded_nv12.data(), width, height, state->incoming_fps);
                return;
            }

            state->nv12.resize(pixels * 3 / 2);
            utils::orient_plane<1>(planes[0], width, height, width, rotation, mirrored, state->nv12.data());
            utils::orient_plane<2>(planes[1], width / 2, height / 2, width, rotation, mirrored, state->nv12.data() + pixels);
            const bool swapped = rotation == 90 || rotation == 270;
            state->channel.write(state->nv12.data(), swapped ? height : width, swapped ? width : height, state->incoming_fps);

        } catch (const std::exception& error) {
            state->fail(error.what());
        }

        #else
        (void)frame;
        (void)rotation;
        (void)mirrored;
        #endif

    }

    std::string virtual_camera::notice() const {

        #ifdef _WIN32
        std::lock_guard lock(state->control);

        return state->installation_notice;
        #else
        return {};
        #endif

    }

    int virtual_camera::install_source(bool remove) {

        #ifdef _WIN32
        unsigned stage = 1;
        std::wstring copied_file;
        bool registered = false;

        try {

            SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
            PSID administrators = nullptr;
            BOOL elevated = FALSE;

            if (AllocateAndInitializeSid(
                    &authority,
                    2,
                    SECURITY_BUILTIN_DOMAIN_RID,
                    DOMAIN_ALIAS_RID_ADMINS,

                    0,
                    0,
                    0,
                    0,
                    0,

                    0,
                    &administrators
                )) {

                CheckTokenMembership(nullptr, administrators, &elevated);
                FreeSid(administrators);

            }

            webcam::utils::install_check(elevated, ERROR_ELEVATION_REQUIRED);
            const auto target = webcam::program_directory(FOLDERID_ProgramFiles);

            if (remove) {

                stage = 7;
                HKEY classes = nullptr;
                const LSTATUS opened = RegOpenKeyExW(
                    HKEY_LOCAL_MACHINE,
                    LR"(Software\Classes\CLSID)",

                    0,
                    KEY_WRITE | KEY_READ | DELETE | KEY_WOW64_64KEY,
                    &classes
                );
                webcam::utils::install_check(opened == ERROR_SUCCESS, opened);

                const LSTATUS removed = RegDeleteTreeW(classes, webcam::source_clsid);
                RegCloseKey(classes);
                webcam::utils::install_check(removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND, removed);
                stage = 9;
                webcam::utils::refresh_camera_host();
                stage = 8;

                return webcam::utils::clean_camera_files(target) ? ERROR_SUCCESS_REBOOT_REQUIRED : 0;

            }

            stage = 2;

            if (!CreateDirectoryW(target.c_str(), nullptr))
                webcam::utils::install_check(GetLastError() == ERROR_ALREADY_EXISTS);

            stage = 3;
            const auto source = utils::executable_directory() + LR"(\camdroid_camera.dll)";
            copied_file = target + L"\\camdroid_camera-" + webcam::new_identifier() + L".dll";
            webcam::utils::install_check(CopyFileW(source.c_str(), copied_file.c_str(), TRUE));
            stage = 4;

            const auto root = webcam::program_directory(FOLDERID_ProgramData);
            webcam::security access(
                L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;FRFX;;;LS)(A;;0x1200ad;;;BU)(A;OICIIO;FA;;;CO)" +
                webcam::frame_server_access()
            );

            if (!CreateDirectoryW(root.c_str(), &access.attributes))
                webcam::utils::install_check(GetLastError() == ERROR_ALREADY_EXISTS);

            stage = 5;
            const HMODULE dll =
                LoadLibraryExW(copied_file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            webcam::utils::install_check(dll != nullptr);
            using registration = HRESULT(WINAPI*)();

            const auto function = reinterpret_cast<registration>(GetProcAddress(dll, "DllRegisterServer"));
            const HRESULT result = function ? function() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            FreeLibrary(dll);
            stage = 6;

            webcam::utils::install_check(
                SUCCEEDED(result),
                HRESULT_FACILITY(result) == FACILITY_WIN32 ? HRESULT_CODE(result) : ERROR_GEN_FAILURE
            );
            registered = true;

            stage = 9;
            webcam::utils::refresh_camera_host();
            stage = 8;

            return webcam::utils::clean_camera_files(target, copied_file) ? ERROR_SUCCESS_REBOOT_REQUIRED : 0;

        } catch (const std::system_error& error) {

            if (!registered && !copied_file.empty())
                DeleteFileW(copied_file.c_str());

            OutputDebugStringA(error.what());
            return static_cast<int>((stage << 16) | (error.code().value() & 0xffff));

        } catch (const std::exception& error) {

            if (!registered && !copied_file.empty())
                DeleteFileW(copied_file.c_str());

            OutputDebugStringA(error.what());
            return static_cast<int>((stage << 16) | ERROR_GEN_FAILURE);

        }

        #else
        return 1;
        #endif

    }

};