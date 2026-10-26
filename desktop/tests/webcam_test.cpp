#include "camera_shared.h"
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <wrl/client.h>
#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv) {

    using namespace camdroid::webcam;

    if (argc != 2)
        return 1;

    HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (FAILED(apartment))
        return 1;

    HMODULE module = nullptr;
    bool started = false;

    try {

        check(MFStartup(MF_VERSION), "MFStartup");
        started = true;
        module = LoadLibraryA(argv[1]);

        if (!module)
            throw std::runtime_error("Cannot load webcam DLL.");

        const auto create = reinterpret_cast<LPFNGETCLASSOBJECT>(GetProcAddress(module, "DllGetClassObject"));
        const auto unload = reinterpret_cast<LPFNCANUNLOADNOW>(GetProcAddress(module, "DllCanUnloadNow"));

        if (!create || !unload)
            throw std::runtime_error("Missing COM entry points.");

        wchar_t temporary[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temporary);
        const std::wstring root = std::wstring(temporary) + L"CamDroid-camera-test-" + new_identifier();

        if (!CreateDirectoryW(root.c_str(), nullptr))
            throw std::runtime_error("Cannot create test directory.");

        {

            frame_channel producer;
            producer.create(root);
            std::vector<std::uint8_t> pixels(image_size, 90);
            std::fill(pixels.begin(), pixels.begin() + width * height, 89);
            producer.write(pixels.data());
            ComPtr<IClassFactory> factory;

            check(create(source_id, IID_PPV_ARGS(&factory)), "Class factory");
            ComPtr<IMFActivate> activation;

            check(factory->CreateInstance(nullptr, IID_PPV_ARGS(&activation)), "Create activation");
            check(activation->SetString(ipc_attribute, producer.path.c_str()), "Set channel");
            ComPtr<IMFMediaSource> source;

            check(activation->ActivateObject(IID_PPV_ARGS(&source)), "Activate source");
            ComPtr<IMFSourceReader> reader;

            check(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader), "Create source reader");
            ComPtr<IMFMediaType> type;

            check(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 1, &type), "Enumerate type");
            UINT32 w = 0, h = 0;
            check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h), "Read dimensions");

            if (w != width || h != height)
                throw std::runtime_error("Wrong camera dimensions.");

            bool high_resolution = false, high_rate = false;

            for (DWORD index = 0;; index++) {

                ComPtr<IMFMediaType> advertised;

                const HRESULT result = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, &advertised);

                if (result == MF_E_NO_MORE_TYPES)
                    break;

                check(result, "Enumerate formats");
                UINT32 x = 0, y = 0, numerator = 0, divisor = 0;
                check(MFGetAttributeSize(advertised.Get(), MF_MT_FRAME_SIZE, &x, &y), "Read advertised size");
                check(MFGetAttributeRatio(advertised.Get(), MF_MT_FRAME_RATE, &numerator, &divisor), "Read advertised rate");

                if (x == 3840 && y == 2160)
                    high_resolution = true;

                if (numerator == 240 && divisor == 1)
                    high_rate = true;

            }

            if (!high_resolution || !high_rate)
                throw std::runtime_error("Webcam still restricts output formats.");

            check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get()), "Select NV12");
            UINT32 rate = 0, denominator = 0;
            check(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &rate, &denominator), "Read camera FPS");

            if (rate != 60 || denominator != 1)
                throw std::runtime_error("60 FPS is not advertised.");

            LONGLONG previous = -1;

            for (int i = 0; i < 5; i++) {

                ComPtr<IMFSample> sample;

                DWORD flags = 0;
                LONGLONG timestamp = 0;
                check(
                    reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestamp, &sample),
                    "Read sample"
                );

                if (!sample || timestamp <= previous)
                    throw std::runtime_error("Missing frame or nonmonotonic clock.");

                previous = timestamp;
                ComPtr<IMFMediaBuffer> buffer;

                check(sample->ConvertToContiguousBuffer(&buffer), "Read frame storage");
                BYTE* data = nullptr;
                DWORD length = 0;
                check(buffer->Lock(&data, nullptr, &length), "Lock sample");
                const bool valid = length == image_size && data[0] == 89 && data[width * height] == 90;
                buffer->Unlock();

                if (!valid)
                    throw std::runtime_error("Camera did not deliver the published pixels.");

            }

            const auto requested = producer.take_request();

            if (!requested || requested->width != width || requested->height != height || requested->fps != 60)
                throw std::runtime_error("Consumer format selection did not reach the producer.");

            std::vector<std::uint8_t> large(1920 * 1080 * 3 / 2, 90);
            std::fill(large.begin(), large.begin() + 1920 * 1080, 89);
            producer.write(large.data(), 1920, 1080);
            std::vector<std::uint8_t> converted(image_size);
            frame_channel consumer;

            if (!consumer.open(producer.path) || !consumer.read(converted.data()) || converted[0] != 89 ||
                converted[width * height] != 90)
                throw std::runtime_error("Changing native resolution did not preserve valid output.");

            consumer.close();
            producer.write(nullptr);
            bool black = false;

            for (int i = 0; i < 6 && !black; i++) {

                ComPtr<IMFSample> sample;

                DWORD flags = 0;
                LONGLONG timestamp = 0;
                check(
                    reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestamp, &sample),
                    "Read offline frame"
                );

                if (!sample)
                    continue;

                ComPtr<IMFMediaBuffer> buffer;

                check(sample->ConvertToContiguousBuffer(&buffer), "Offline storage");
                BYTE* data = nullptr;
                check(buffer->Lock(&data, nullptr, nullptr), "Lock offline frame");
                black = data[0] == 16 && data[width * height] == 128;
                buffer->Unlock();

            }

            if (!black)
                throw std::runtime_error("Stopped video did not become an offline frame.");

            reader.Reset();
            check(source->Shutdown(), "Source shutdown");
            activation->ShutdownObject();
            source.Reset();
            activation.Reset();
            factory.Reset();

            if (unload() != S_OK)
                throw std::runtime_error("COM objects remain alive after shutdown.");

        }

        RemoveDirectoryW(root.c_str());
        FreeLibrary(module);
        MFShutdown();
        CoUninitialize();
        std::cout << "Webcam COM activation, IPC pixels, NV12 samples, pacing, offline frames and shutdown passed.\n";

        return 0;

    } catch (const std::exception& error) {

        std::cerr << error.what() << '\n';
        // Leave the loaded module resident on failure: live async objects must
        // never execute into an unloaded DLL while the test process exits.
        if (started)
            MFShutdown();

        CoUninitialize();

        return 1;

    }

}