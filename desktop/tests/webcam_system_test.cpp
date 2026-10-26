#include "virtual_camera.h"
#include "camera_shared.h"
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <mfreadwrite.h>

#include <wrl/client.h>
#include <chrono>
#include <thread>
#include <iostream>

using Microsoft::WRL::ComPtr;

int main() {

    using namespace camdroid;
    using namespace camdroid::webcam;
    bool foundation = false;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (FAILED(apartment))
        return 1;

    try {

        virtual_camera controller;

        for (int attempt = 0; attempt < 2; attempt++) {

            controller.enable(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);

            while (controller.pending() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(25));

            if (!controller.active())
                throw std::runtime_error(controller.error());

            controller.enable(false);

            while (controller.pending() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(25));

            if (controller.active())
                throw std::runtime_error("System camera did not stop.");

        }

        check(MFStartup(MF_VERSION), "Start integration test");
        foundation = true;
        frame_channel producer;
        producer.create();
        constexpr int w = 1920, h = 1080, rate = 60;

        std::vector<std::uint8_t> pixels(w * h * 3 / 2, 90);
        std::fill(pixels.begin(), pixels.begin() + w * h, 89);
        producer.write(pixels.data(), w, h, rate);

        std::jthread publishing([&](std::stop_token stop) {

            while (!stop.stop_requested()) {

                producer.write(pixels.data(), w, h, rate);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));

            }

        });

        struct camera_guard {

            ComPtr<IMFVirtualCamera> camera;

            ~camera_guard() {

                if (camera) {

                    camera->Stop();
                    camera->Remove();
                    camera->Shutdown();

                }

            }

        } guard;

        check(
            MFCreateVirtualCamera(
                MFVirtualCameraType_SoftwareCameraSource,
                MFVirtualCameraLifetime_Session,
                MFVirtualCameraAccess_CurrentUser,

                L"CamDroid integration test",
                source_clsid,
                nullptr,
                0,

                &guard.camera
            ),
            "Create system test camera"
        );
        check(guard.camera->SetString(ipc_attribute, producer.path.c_str()), "Set system test channel");

        check(guard.camera->Start(nullptr), "Start system test camera");
        wchar_t* symbolic_link = nullptr;
        UINT32 link_length = 0;
        check(
            guard.camera
                ->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symbolic_link, &link_length),

            "Get shared camera identity"
        );
        ComPtr<IMFAttributes> attributes;

        check(MFCreateAttributes(&attributes, 2), "Create app attributes");
        check(
            attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
            "Select camera category"
        );

        const HRESULT link_result =
            attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, symbolic_link);
        CoTaskMemFree(symbolic_link);
        check(link_result, "Select camera for apps");
        ComPtr<IMFMediaSource> source;

        check(MFCreateDeviceSource(attributes.Get(), &source), "Open first camera app");
        ComPtr<IMFSourceReader> reader;

        check(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader), "Create Windows camera reader");
        ComPtr<IMFMediaType> type;

        check(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &type), "Get preferred camera type");
        UINT32 actual_w = 0, actual_h = 0, numerator = 0, denominator = 0;
        check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &actual_w, &actual_h), "Read preferred dimensions");
        check(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &numerator, &denominator), "Read preferred rate");

        if (actual_w != w || actual_h != h || numerator != rate || denominator != 1)
            throw std::runtime_error("Windows did not receive the native 1080p/60 preferred format.");

        check(
            reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get()),
            "Select preferred camera type"
        );
        int received = 0;

        for (int attempt = 0; attempt < 40 && received < 5; attempt++) {

            producer.write(pixels.data(), w, h, rate);
            ComPtr<IMFSample> sample;

            DWORD flags = 0;
            LONGLONG timestamp = 0;
            check(
                reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestamp, &sample),
                "Read Windows camera frame"
            );

            if (!sample) {

                if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))
                    throw std::runtime_error("Windows camera stopped delivering frames; flags=" + std::to_string(flags));

                continue;

            }

            received++;
            ComPtr<IMFMediaBuffer> buffer;

            check(sample->ConvertToContiguousBuffer(&buffer), "Get Windows camera pixels");
            BYTE* data = nullptr;
            DWORD length = 0;
            check(buffer->Lock(&data, nullptr, &length), "Lock Windows camera pixels");
            const bool valid = length == pixels.size() && data[0] == 89 && data[w * h] == 90;
            buffer->Unlock();

            if (!valid)
                throw std::runtime_error("Windows camera did not deliver the published pixels.");

        }

        if (received != 5)
            throw std::runtime_error("Windows returned no complete video frames.");

        ComPtr<IMFMediaSource> second_source;

        check(attributes->SetUINT32(MF_DEVSOURCE_ATTRIBUTE_FRAMESERVER_SHARE_MODE, 1), "Share camera with first app");
        check(MFCreateDeviceSource(attributes.Get(), &second_source), "Open second simultaneous camera app");
        ComPtr<IMFSourceReader> second_reader;
        ComPtr<IMFAttributes> reader_attributes;

        check(MFCreateAttributes(&reader_attributes, 1), "Create second reader attributes");
        check(
            reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE),
            "Enable app output conversion"
        );

        check(
            MFCreateSourceReaderFromMediaSource(second_source.Get(), reader_attributes.Get(), &second_reader),
            "Create second app reader"
        );

        {

            ComPtr<IMFMediaType> output;

            check(MFCreateMediaType(&output), "Create second app output");
            check(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set app video type");
            check(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Set app pixel format");
            check(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, 640, 480), "Set app output size");
            check(MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, 30, 1), "Set app output rate");
            check(second_reader->SetCurrentMediaType(0, nullptr, output.Get()), "Select second app output");

        }

        const auto read_client = [&](IMFSourceReader* client, int expected_w, int expected_h, int expected_fps) {

            for (int attempt = 0; attempt < 30; attempt++) {

                producer.write(pixels.data(), w, h, rate);
                ComPtr<IMFSample> sample;

                DWORD flags = 0;
                LONGLONG time = 0;
                check(client->ReadSample(0, 0, nullptr, &flags, &time, &sample), "Read simultaneous app frame");

                if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))
                    throw std::runtime_error("Shared camera client failed.");

                if (!sample)
                    continue;

                ComPtr<IMFMediaType> current;

                check(client->GetCurrentMediaType(0, &current), "Read app-specific format");
                UINT32 x = 0, y = 0, fps = 0, divisor = 0;
                check(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &x, &y), "Check app dimensions");
                check(MFGetAttributeRatio(current.Get(), MF_MT_FRAME_RATE, &fps, &divisor), "Check app FPS");

                if (x != expected_w || y != expected_h || fps != expected_fps || divisor != 1)
                    throw std::runtime_error(
                        "App output expected " + std::to_string(expected_w) + "x" + std::to_string(expected_h) + "@" +
                        std::to_string(expected_fps) + ", got " + std::to_string(x) + "x" + std::to_string(y) + "@" +
                        std::to_string(fps) + "/" + std::to_string(divisor)
                    );

                ComPtr<IMFMediaBuffer> buffer;

                check(sample->ConvertToContiguousBuffer(&buffer), "Read app frame storage");
                BYTE* data = nullptr;
                DWORD length = 0;
                check(buffer->Lock(&data, nullptr, &length), "Lock app frame");
                const auto center = data[(expected_h / 2) * expected_w + expected_w / 2];

                const bool valid = length == expected_w * expected_h * 3 / 2 && center == 89;
                buffer->Unlock();

                if (!valid)
                    throw std::runtime_error(
                        "Shared camera frame pixels are invalid: " + std::to_string(expected_w) + "x" +
                        std::to_string(expected_h) + ", bytes=" + std::to_string(length) +
                        ", center=" + std::to_string(center)
                    );

                return;

            }

            throw std::runtime_error("Shared camera client returned no complete frame.");

        };

        for (int i = 0; i < 3; i++) {

            read_client(reader.Get(), 1920, 1080, 60);
            read_client(second_reader.Get(), 640, 480, 30);

        }

        if (producer.native_format() != std::optional<protocol::video_format>{{1920, 1080, 60}})
            throw std::runtime_error("Consuming apps changed capture settings.");

        second_reader.Reset();
        second_source.Reset();
        reader.Reset();
        source.Reset();

        std::cout
            << "Real Windows camera: repeated start/stop, preferred 1080p/60 format published pixels and two simultaneous "
            "app output formats passed.\n";

    } catch (const std::exception& error) {

        std::cerr << error.what() << '\n';

        if (foundation)
            MFShutdown();

        CoUninitialize();

        return 1;

    }

    if (foundation)
        MFShutdown();

    CoUninitialize();

    return 0;

}