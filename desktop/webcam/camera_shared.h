#ifndef CAMDROID_CAMERA_SHARED_H
#define CAMDROID_CAMERA_SHARED_H
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <cstdint>
#include <string>

#include <stdexcept>
#include <cstring>
#include <vector>
#include <optional>
#include <algorithm>

#include <sstream>
#include <iomanip>
#include <system_error>
#include "video_control.h"

namespace camdroid::webcam {

    inline constexpr GUID source_id{0x65f8632a, 0xe539, 0x48ed, {0xb3, 0x54, 0x24, 0xa2, 0xd5, 0x37, 0x1e, 0x94}};
    inline constexpr GUID ipc_attribute{0x150ee912, 0x6815, 0x48a9, {0x8f, 0x58, 0xb0, 0x5b, 0x2a, 0xd1, 0xee, 0x70}};
    inline constexpr wchar_t source_clsid[] = L"{65F8632A-E539-48ED-B354-24A2D5371E94}";
    inline constexpr int width = 1280;
    inline constexpr int height = 720;

    inline constexpr int fps = 30;
    inline constexpr DWORD image_size = width * height * 3 / 2;
    inline constexpr LONGLONG duration = 10000000 / fps;

    inline constexpr DWORD capacity = 3840 * 2160 * 3 / 2;

    struct shared_frame {

        std::uint32_t magic;
        std::uint32_t version;
        std::uint32_t active;
        std::uint32_t reserved;
        std::uint64_t updated;

        std::uint64_t sequence;
        std::uint32_t frame_width;
        std::uint32_t frame_height;
        std::uint64_t request_sequence;
        std::uint64_t request_updated;

        std::uint32_t request_width;
        std::uint32_t request_height;
        std::uint32_t request_fps;
        std::uint32_t frame_fps;
        std::uint8_t pixels[capacity];

    };

    static_assert(offsetof(shared_frame, pixels) == 72);

    inline void check(HRESULT result, const char* operation) {

        if (SUCCEEDED(result))
            return;

        std::ostringstream message;
        message << operation << " (HRESULT 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
                << static_cast<unsigned long>(result) << ")";

        if (HRESULT_FACILITY(result) == FACILITY_WIN32)
            message << ": " << std::system_category().message(HRESULT_CODE(result));

        throw std::runtime_error(message.str());

    }

    inline std::wstring program_directory(REFKNOWNFOLDERID folder) {

        PWSTR path = nullptr;
        check(SHGetKnownFolderPath(folder, 0, nullptr, &path), "Cannot locate camera directory");
        std::wstring result(path);
        CoTaskMemFree(path);

        return result + L"\\CamDroid";

    }

    inline std::wstring new_identifier() {

        GUID id{};
        check(CoCreateGuid(&id), "Cannot create camera identifier");
        wchar_t text[40]{};
        StringFromGUID2(id, text, 40);

        return text;

    }

    class security {

        private:
        PSECURITY_DESCRIPTOR descriptor = nullptr;

        public:
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};

        explicit security(const std::wstring& sddl) {
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
                throw std::runtime_error("Cannot configure camera access permissions.");

            attributes.lpSecurityDescriptor = descriptor;
        }

        ~security() {
            if (descriptor)
                LocalFree(descriptor);
        }

        security(const security&) = delete;
        security& operator=(const security&) = delete;

    };

    inline std::wstring current_user_sid() {

        HANDLE token = nullptr;

        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            throw std::runtime_error("Cannot identify camera user.");

        DWORD length = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &length);
        std::string storage(length, '\0');
        const BOOL valid = GetTokenInformation(token, TokenUser, storage.data(), length, &length);
        CloseHandle(token);

        if (!valid)
            throw std::runtime_error("Cannot read camera user.");

        LPWSTR text = nullptr;

        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &text))
            throw std::runtime_error("Cannot format camera user.");

        std::wstring result(text);
        LocalFree(text);

        return result;

    }

    inline std::wstring frame_server_access() {

        DWORD sid_size = 0, domain_size = 0;
        SID_NAME_USE use{};
        const wchar_t* account = LR"(NT SERVICE\FrameServer)";
        LookupAccountNameW(nullptr, account, nullptr, &sid_size, nullptr, &domain_size, &use);

        if (!sid_size)
            return {};

        std::string sid(sid_size, '\0');
        std::wstring domain(domain_size, L'\0');

        if (!LookupAccountNameW(nullptr, account, sid.data(), &sid_size, domain.data(), &domain_size, &use))
            return {};

        LPWSTR text = nullptr;

        if (!ConvertSidToStringSidW(sid.data(), &text))
            return {};

        const std::wstring result = std::wstring(L"(A;;FA;;;") + text + L")";
        LocalFree(text);

        return result;

    }

    class frame_channel {

        private:
        HANDLE file = INVALID_HANDLE_VALUE;
        HANDLE mapping = nullptr;
        HANDLE mutex = nullptr;
        shared_frame* view = nullptr;

        std::wstring directory;
        bool writable = false;
        std::uint64_t observed_request = 0;
        std::vector<std::uint8_t> snapshot;

        public:
        std::wstring path;
        DWORD open_error = ERROR_SUCCESS;
        const char* open_stage = "";
        frame_channel() = default;

        frame_channel(const frame_channel&) = delete;
        frame_channel& operator=(const frame_channel&) = delete;

        ~frame_channel() {
            close();
        }

        void close() noexcept {

            if (view)
                UnmapViewOfFile(view);

            if (mapping)
                CloseHandle(mapping);

            if (file != INVALID_HANDLE_VALUE)
                CloseHandle(file);

            if (mutex)
                CloseHandle(mutex);

            view = nullptr;
            mapping = mutex = nullptr;
            file = INVALID_HANDLE_VALUE;

            if (writable && !path.empty())
                DeleteFileW(path.c_str());

            if (!directory.empty())
                RemoveDirectoryW(directory.c_str());

            writable = false;
            directory.clear();
            path.clear();
            observed_request = 0;

        }

        void create(const std::wstring& root = program_directory(FOLDERID_ProgramData)) {

            close();
            const auto id = new_identifier();
            security access(L"D:P(A;;FA;;;SY)(A;;FA;;;LS)(A;;FA;;;" + current_user_sid() + L")" + frame_server_access());
            directory = root + L"\\" + id;

            if (!CreateDirectoryW(directory.c_str(), &access.attributes))
                throw std::runtime_error("Cannot create camera frame directory. Install webcam support first.");

            path = directory + L"\\frames.bin";
            writable = true;
            file = CreateFileW(
                path.c_str(),
                GENERIC_READ | GENERIC_WRITE | DELETE,

                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                &access.attributes,
                CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                nullptr
            );

            if (file == INVALID_HANDLE_VALUE)
                throw std::runtime_error("Cannot create webcam frame storage.");

            LARGE_INTEGER length{};
            length.QuadPart = sizeof(shared_frame);

            if (!SetFilePointerEx(file, length, nullptr, FILE_BEGIN) || !SetEndOfFile(file))
                throw std::runtime_error("Cannot size webcam frame storage.");

            mapping = CreateFileMappingW(file, &access.attributes, PAGE_READWRITE, 0, sizeof(shared_frame), nullptr);

            if (!mapping)
                throw std::runtime_error("Cannot map webcam frame storage.");

            view = static_cast<shared_frame*>(
                MapViewOfFile(mapping, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(shared_frame))
            );

            if (!view)
                throw std::runtime_error("Cannot access webcam frame storage.");

            mutex = CreateMutexW(&access.attributes, FALSE, (L"Global\\CamDroid-" + id).c_str());

            if (!mutex)
                throw std::runtime_error("Cannot create webcam frame lock.");

            writable = true;
            std::memset(view, 0, sizeof(*view));
            view->magic = 0x43444D46;
            view->version = 2;
            view->reserved = 1; // Per-client output conversion is allowed until a user overrides it.

        }

        bool open(const std::wstring& frame_path) {

            close();

            const auto failed = [&](const char* stage, DWORD error) {

                open_stage = stage;
                open_error = error ? error : ERROR_GEN_FAILURE;
                close();

                return false;

            };

            if (frame_path.empty())
                return failed("Frame channel path", ERROR_INVALID_NAME);

            const auto slash = frame_path.find_last_of(L"\\/");

            if (slash == std::wstring::npos || slash < 38)
                return failed("Frame channel identifier", ERROR_INVALID_NAME);

            const auto id = frame_path.substr(slash - 38, 38);
            GUID parsed{};

            if (FAILED(CLSIDFromString(id.c_str(), &parsed)))
                return failed("Frame channel identifier", ERROR_INVALID_NAME);

            file = CreateFileW(
                frame_path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,

                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );

            if (file == INVALID_HANDLE_VALUE)
                return failed("Open frame storage", GetLastError());

            LARGE_INTEGER length{};

            if (!GetFileSizeEx(file, &length))
                return failed("Read frame storage size", GetLastError());

            if (length.QuadPart != sizeof(shared_frame))
                return failed("Frame storage version", ERROR_REVISION_MISMATCH);

            mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);

            if (!mapping)
                return failed("Map frame storage", GetLastError());

            view = static_cast<shared_frame*>(
                MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(shared_frame))
            );

            if (!view)
                return failed("Access frame storage", GetLastError());

            mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (L"Global\\CamDroid-" + id).c_str());

            if (!mutex)
                return failed("Open frame synchronization", GetLastError());

            writable = false;
            open_error = ERROR_SUCCESS;
            open_stage = "";

            return true;

        }

        void write(const std::uint8_t* pixels, int w = width, int h = height, int rate = fps) {

            if (!view || !writable || w < 2 || h < 2 || w % 2 || h % 2 || w > 3840 || h > 3840 ||
                static_cast<std::uint64_t>(w) * h * 3 / 2 > capacity)
                return;

            const DWORD result = WaitForSingleObject(mutex, 0);

            if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
                return;

            if (pixels)
                std::memcpy(view->pixels, pixels, static_cast<std::size_t>(w) * h * 3 / 2);

            view->frame_width = w;
            view->frame_height = h;
            view->frame_fps = rate;
            view->active = pixels != nullptr;
            view->updated = GetTickCount64();

            view->sequence++;
            ReleaseMutex(mutex);

        }

        void allow_app_outputs(bool allow) {

            if (!view || !mutex || !writable)
                return;

            const DWORD result = WaitForSingleObject(mutex, 5);

            if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
                return;

            view->reserved = allow ? 1 : 0;
            ReleaseMutex(mutex);

        }

        bool app_outputs_allowed() {

            if (!view || !mutex)
                return false;

            const DWORD result = WaitForSingleObject(mutex, 5);

            if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
                return false;

            const bool allowed = (view->reserved & 1) != 0;
            ReleaseMutex(mutex);

            return allowed;

        }

        void request_format(protocol::video_format format, bool force = false) {

            if (!view || !mutex)
                return;

            const DWORD result = WaitForSingleObject(mutex, 5);

            if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
                return;

            if (force || view->request_width != format.width || view->request_height != format.height ||
                view->request_fps != format.fps) {

                view->request_width = format.width;
                view->request_height = format.height;
                view->request_fps = format.fps;
                view->request_sequence++;

            }

            view->request_updated = GetTickCount64();
            ReleaseMutex(mutex);

        }

        std::optional<protocol::video_format> take_request() {

            if (!view || !mutex)
                return {};

            const DWORD result = WaitForSingleObject(mutex, 0);

            if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
                return {};

            std::optional<protocol::video_format> format;

            if (view->request_sequence != observed_request && GetTickCount64() - view->request_updated < 2000) {

                observed_request = view->request_sequence;

                if (view->request_width >= 2 && view->request_width <= 3840 && view->request_height >= 2 &&
                    view->request_height <= 3840 && !(view->request_width % 2) && !(view->request_height % 2) &&
                    static_cast<std::uint64_t>(view->request_width) * view->request_height <= 3840ULL * 2160 &&
                    view->request_fps >= 1 && view->request_fps <= 255)
                    format = protocol::video_format{
                        static_cast<std::uint16_t>(view->request_width),

                        static_cast<std::uint16_t>(view->request_height),
                        static_cast<std::uint16_t>(view->request_fps)
                    };

            }

            ReleaseMutex(mutex);

            return format;

        }

        std::optional<protocol::video_format> native_format() {

            if (!view || !mutex)
                return {};

            const DWORD result = WaitForSingleObject(mutex, 5);

            if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
                return {};

            std::optional<protocol::video_format> format;

            if (view->magic == 0x43444D46 && view->version == 2 && view->active == 1 &&
                GetTickCount64() - view->updated < 2000 && view->frame_width >= 2 && view->frame_width <= 3840 &&
                view->frame_height >= 2 && view->frame_height <= 3840 && !(view->frame_width % 2) &&
                !(view->frame_height % 2) &&
                static_cast<std::uint64_t>(view->frame_width) * view->frame_height <= 3840ULL * 2160 && view->frame_fps > 0 &&
                view->frame_fps <= 255)

                format = protocol::video_format{
                    static_cast<std::uint16_t>(view->frame_width),
                    static_cast<std::uint16_t>(view->frame_height),
                    static_cast<std::uint16_t>(view->frame_fps)
                };

            ReleaseMutex(mutex);

            return format;

        }

        bool read(std::uint8_t* pixels, int output_width = width, int output_height = height) {

            if (!view || !mutex)
                return false;

            if (snapshot.size() != capacity) {

                try {

                    snapshot.resize(capacity);

                } catch (...) {

                    return false;

                }

            }

            const DWORD result = WaitForSingleObject(mutex, 5);

            if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
                return false;

            const int w = static_cast<int>(view->frame_width), h = static_cast<int>(view->frame_height);
            const bool valid = view->magic == 0x43444D46 && view->version == 2 && view->active == 1 &&
                               GetTickCount64() - view->updated < 2000 && w >= 2 && h >= 2 && w <= 3840 && h <= 3840 &&
                               !(w % 2) && !(h % 2) && static_cast<std::uint64_t>(w) * h * 3 / 2 <= capacity;

            if (valid)
                std::memcpy(snapshot.data(), view->pixels, static_cast<std::size_t>(w) * h * 3 / 2);

            ReleaseMutex(mutex);

            if (!valid)
                return false;

            if (w == output_width && h == output_height) {

                std::memcpy(pixels, snapshot.data(), static_cast<std::size_t>(w) * h * 3 / 2);

                return true;

            }

            std::memset(pixels, 16, output_width * output_height);
            std::memset(pixels + output_width * output_height, 128, output_width * output_height / 2);
            const double ratio = std::min(static_cast<double>(output_width) / w, static_cast<double>(output_height) / h);
            const int fw = std::max(2, static_cast<int>(w * ratio) & ~1), fh = std::max(2, static_cast<int>(h * ratio) & ~1);
            const int left = ((output_width - fw) / 2) & ~1, top = ((output_height - fh) / 2) & ~1;

            for (int y = 0; y < fh; y++)
                for (int x = 0; x < fw; x++)
                    pixels[(top + y) * output_width + left + x] = snapshot[(y * h / fh) * w + x * w / fw];

            for (int y = 0; y < fh / 2; y++)
                for (int x = 0; x < fw / 2; x++) {

                    const int from = w * h + (y * (h / 2) / (fh / 2)) * w + (x * (w / 2) / (fw / 2)) * 2;
                    const int to = output_width * output_height + (top / 2 + y) * output_width + left + x * 2;
                    pixels[to] = snapshot[from];
                    pixels[to + 1] = snapshot[from + 1];

                }

            return true;

        }

    };

};

#endif