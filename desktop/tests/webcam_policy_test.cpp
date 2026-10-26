#include "camera_shared.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <wrl/client.h>
#include <iostream>

using Microsoft::WRL::ComPtr;

namespace camdroid::tests {

    using namespace webcam;

    struct client {

        ComPtr<IMFActivate> activation;
        ComPtr<IMFMediaSource> source;
        ComPtr<IMFSourceReader> reader;

        ~client() {

            reader.Reset();

            if (source)
                source->Shutdown();

            if (activation)
                activation->ShutdownObject();

        }

        void open(IClassFactory* factory, const std::wstring& path, int w, int h, int fps) {

            check(factory->CreateInstance(nullptr, IID_PPV_ARGS(&activation)), "Create client");
            check(activation->SetString(ipc_attribute, path.c_str()), "Set client channel");
            check(activation->ActivateObject(IID_PPV_ARGS(&source)), "Activate client");
            check(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader), "Create client reader");

            for (DWORD index = 0;; index++) {

                ComPtr<IMFMediaType> type;

                const HRESULT result = reader->GetNativeMediaType(0, index, &type);

                if (result == MF_E_NO_MORE_TYPES)
                    throw std::runtime_error("Requested client format is not advertised.");

                check(result, "Enumerate client format");
                UINT32 x = 0, y = 0, rate = 0, divisor = 0;
                check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &x, &y), "Read client size");
                check(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &rate, &divisor), "Read client rate");

                if (x == w && y == h && rate == fps && divisor == 1) {

                    check(reader->SetCurrentMediaType(0, nullptr, type.Get()), "Select client format");

                    return;

                }

            }

        }

        bool read(int w, int h, int fps) {

            ComPtr<IMFSample> sample;

            DWORD flags = 0;
            LONGLONG timestamp = 0;
            check(reader->ReadSample(0, 0, nullptr, &flags, &timestamp, &sample), "Read client frame");

            if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))
                throw std::runtime_error("Client stream failed.");

            if (!sample)
                return false;

            ComPtr<IMFMediaType> type;

            check(reader->GetCurrentMediaType(0, &type), "Read current client format");
            UINT32 x = 0, y = 0, rate = 0, divisor = 0;
            check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &x, &y), "Get client dimensions");
            check(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &rate, &divisor), "Get client FPS");

            if (x != w || y != h || rate != fps || divisor != 1)
                return false;

            ComPtr<IMFMediaBuffer> buffer;

            check(sample->ConvertToContiguousBuffer(&buffer), "Read client pixels");
            BYTE* data = nullptr;
            DWORD length = 0;
            check(buffer->Lock(&data, nullptr, &length), "Lock client pixels");
            const bool valid = length == w * h * 3 / 2 && data[(h / 2) * w + w / 2] == 89;
            buffer->Unlock();

            return valid;

        }

    };

};

int main(int argc, char** argv) {

    using namespace camdroid;
    using namespace webcam;

    if (argc != 2 || FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return 1;

    HMODULE module = nullptr;
    bool foundation = false;

    try {

        check(MFStartup(MF_VERSION), "Start policy test");
        foundation = true;
        module = LoadLibraryA(argv[1]);

        if (!module)
            throw std::runtime_error("Cannot load source DLL.");

        const auto create = reinterpret_cast<LPFNGETCLASSOBJECT>(GetProcAddress(module, "DllGetClassObject"));

        if (!create)
            throw std::runtime_error("Missing source class factory.");

        wchar_t temporary[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temporary);
        const auto root = std::wstring(temporary) + L"CamDroid-policy-test-" + new_identifier();

        if (!CreateDirectoryW(root.c_str(), nullptr))
            throw std::runtime_error("Cannot create test directory.");

        {

            frame_channel producer;
            producer.create(root);
            std::vector<std::uint8_t> pixels(1920 * 1080 * 3 / 2, 90);
            std::fill(pixels.begin(), pixels.begin() + 1920 * 1080, 89);
            producer.write(pixels.data(), 1920, 1080, 60);
            ComPtr<IClassFactory> factory;

            check(create(source_id, IID_PPV_ARGS(&factory)), "Get source factory");
            tests::client first, second;
            first.open(factory.Get(), producer.path, 1280, 720, 30);
            second.open(factory.Get(), producer.path, 640, 480, 15);

            for (int i = 0; i < 4; i++) {

                producer.write(pixels.data(), 1920, 1080, 60);

                if (!first.read(1280, 720, 30) || !second.read(640, 480, 15))
                    throw std::runtime_error("Clients interfered with each other's output format.");

            }

            const auto native = producer.native_format();

            if (!native || *native != protocol::video_format{1920, 1080, 60})
                throw std::runtime_error("App changed capture settings.");

            producer.allow_app_outputs(false);
            bool first_native = false, second_native = false;

            for (int i = 0; i < 16 && (!first_native || !second_native); i++) {

                producer.write(pixels.data(), 1920, 1080, 60);
                first_native = first.read(1920, 1080, 60) || first_native;
                second_native = second.read(1920, 1080, 60) || second_native;

            }

            if (!first_native || !second_native)
                throw std::runtime_error("User format did not take priority over both clients.");

            tests::client native_client;
            native_client.open(factory.Get(), producer.path, 1920, 1080, 60);
            ComPtr<IMFMediaType> extra;

            if (native_client.reader->GetNativeMediaType(0, 1, &extra) != MF_E_NO_MORE_TYPES)
                throw std::runtime_error("App-specific formats remain advertised while disabled.");

        }

        RemoveDirectoryW(root.c_str());
        FreeLibrary(module);
        MFShutdown();
        CoUninitialize();
        std::cout << "Independent clients, unchanged capture and user-priority format switching passed.\n";

        return 0;

    } catch (const std::exception& error) {

        std::cerr << error.what() << '\n';

        if (foundation)
            MFShutdown();

        CoUninitialize();

        return 1;

    }

}