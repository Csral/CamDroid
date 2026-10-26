#include "camera_shared.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <ks.h>

#include <ksmedia.h>
#include <ksproxy.h>
#include <wrl.h>
#include <atomic>
#include <chrono>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#define CAMERA_TRY(call)                                                                                                     \
    do {                                                                                                                     \
        const HRESULT result = (call);                                                                                       \
        if (FAILED(result))                                                                                                  \
            return result;                                                                                                   \
    } while (false)

namespace camdroid::webcam {

    using Microsoft::WRL::ComPtr;
    using Microsoft::WRL::RuntimeClass;
    using Microsoft::WRL::RuntimeClassFlags;
    using Microsoft::WRL::ClassicCom;
    using Microsoft::WRL::FtmBase;
    using Microsoft::WRL::ChainInterfaces;

    inline std::atomic<long> objects{0};
    inline std::atomic<long> locks{0};
    inline HMODULE module = nullptr;

    namespace utils {

        void report_activation(const std::wstring& message) {

            const HANDLE log = RegisterEventSourceW(nullptr, L"CamDroid");

            if (!log)
                return;

            const wchar_t* lines[]{message.c_str()};
            ReportEventW(log, EVENTLOG_ERROR_TYPE, 0, 1, nullptr, 1, 0, lines, nullptr);
            DeregisterEventSource(log);

        }

    };

    struct counted {

        counted() {
            objects++;
        }

        ~counted() {
            objects--;
        }

    };

    class camera_stream final :
        public RuntimeClass<
            RuntimeClassFlags<ClassicCom>,
            ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>,
            FtmBase>,
        public counted {

        private:
        std::mutex mutex;
        std::condition_variable changed;

        ComPtr<IMFMediaSource> parent;
        ComPtr<IMFMediaEventQueue> events;
        ComPtr<IMFStreamDescriptor> descriptor;

        std::deque<ComPtr<IUnknown>> requests;
        MF_STREAM_STATE state = MF_STREAM_STATE_STOPPED;
        bool shutdown = false;
        std::uint64_t generation = 0;

        std::jthread worker;
        frame_channel channel;
        protocol::video_format selected;
        bool app_formats = true;

        HRESULT configure_descriptor(protocol::video_format preferred, bool allow_apps) {

            std::vector<protocol::video_format> formats{preferred};
            std::vector<ComPtr<IMFMediaType>> storage;
            std::vector<IMFMediaType*> types;

            const std::pair<int, int> sizes[]{
                {1280, 720},
                {640, 480},
                {1920, 1080},
                {3840, 2160},

                {720, 1280},
                {480, 640},
                {1080, 1920},
                {2160, 3840}
            };

            if (allow_apps)
                for (const auto [w, h] : sizes)
                    for (const int rate : {30, 60, 90, 120, 144, 240, 24, 25, 50, 15, 10, 5, 1}) {

                        const protocol::video_format candidate{
                            static_cast<std::uint16_t>(w),
                            static_cast<std::uint16_t>(h),
                            static_cast<std::uint16_t>(rate)
                        };

                        if (candidate != preferred)
                            formats.push_back(candidate);

                    }

            for (const auto format : formats) {

                const int w = format.width, h = format.height, rate = format.fps;
                ComPtr<IMFMediaType> type;

                CAMERA_TRY(MFCreateMediaType(&type));
                CAMERA_TRY(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
                CAMERA_TRY(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
                CAMERA_TRY(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
                CAMERA_TRY(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));

                CAMERA_TRY(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
                CAMERA_TRY(type->SetUINT32(MF_MT_SAMPLE_SIZE, w * h * 3 / 2));
                CAMERA_TRY(type->SetUINT32(MF_MT_DEFAULT_STRIDE, w));
                CAMERA_TRY(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h));
                CAMERA_TRY(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, rate, 1));

                CAMERA_TRY(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
                CAMERA_TRY(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
                CAMERA_TRY(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
                types.push_back(type.Get());
                storage.push_back(std::move(type));

            }

            ComPtr<IMFStreamDescriptor> updated;

            CAMERA_TRY(MFCreateStreamDescriptor(0, static_cast<DWORD>(types.size()), types.data(), &updated));
            ComPtr<IMFMediaTypeHandler> handler;

            CAMERA_TRY(updated->GetMediaTypeHandler(&handler));
            CAMERA_TRY(handler->SetCurrentMediaType(types.front()));
            CAMERA_TRY(updated->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
            CAMERA_TRY(updated->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0));
            CAMERA_TRY(updated->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));

            CAMERA_TRY(updated->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));
            descriptor = std::move(updated);
            app_formats = allow_apps;

            return S_OK;

        }

        void deliver(std::stop_token token) noexcept {

            struct apartment_guard {

                HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

                ~apartment_guard() {
                    if (SUCCEEDED(result))
                        CoUninitialize();
                }

            } apartment;

            if (FAILED(apartment.result)) {

                events->QueueEventParamVar(MEError, GUID_NULL, apartment.result, nullptr);

                return;

            }

            auto due = std::chrono::steady_clock::now();

            while (!token.stop_requested()) {

                ComPtr<IUnknown> request;

                std::uint64_t epoch;
                protocol::video_format format;

                {

                    std::unique_lock lock(mutex);

                    changed.wait(lock, [&] {
                        return shutdown || token.stop_requested() || (state == MF_STREAM_STATE_RUNNING && !requests.empty());
                    });

                    if (shutdown || token.stop_requested())
                        return;

                    const auto native = channel.native_format();
                    const bool allow_apps = channel.app_outputs_allowed();
                    const bool format_changed = !allow_apps && native && *native != selected;

                    if (format_changed || allow_apps != app_formats) {

                        const auto target = format_changed ? *native : selected;
                        HRESULT result = configure_descriptor(target, allow_apps);

                        if (SUCCEEDED(result) && format_changed) {

                            ComPtr<IMFMediaTypeHandler> handler;
                            ComPtr<IMFMediaType> type;

                            result = descriptor->GetMediaTypeHandler(&handler);

                            if (SUCCEEDED(result))
                                result = handler->GetCurrentMediaType(&type);

                            if (SUCCEEDED(result))
                                result = events->QueueEventParamUnk(MEStreamFormatChanged, GUID_NULL, S_OK, type.Get());

                            if (SUCCEEDED(result))
                                selected = target;

                            due = std::chrono::steady_clock::now();

                        }

                        if (FAILED(result)) {

                            events->QueueEventParamVar(MEError, GUID_NULL, result, nullptr);
                            state = MF_STREAM_STATE_STOPPED;
                            continue;

                        }

                    }

                    epoch = generation;
                    format = selected;

                    if (due < std::chrono::steady_clock::now())
                        due = std::chrono::steady_clock::now();

                    changed.wait_until(lock, due, [&] {
                        return shutdown || generation != epoch || token.stop_requested();
                    });

                    if (shutdown || token.stop_requested())
                        return;

                    if (generation != epoch)
                        continue;

                    request = std::move(requests.front());
                    requests.pop_front();

                }

                const DWORD frame_bytes = static_cast<DWORD>(format.width) * format.height * 3 / 2;
                const LONGLONG frame_duration = 10000000 / format.fps;
                due += std::chrono::nanoseconds(1000000000 / format.fps);
                channel.request_format(format);
                ComPtr<IMFSample> sample;
                ComPtr<IMFMediaBuffer> buffer;

                HRESULT result = MFCreateSample(&sample);

                if (SUCCEEDED(result))
                    result = MFCreateMemoryBuffer(frame_bytes, &buffer);

                BYTE* pixels = nullptr;

                if (SUCCEEDED(result))
                    result = buffer->Lock(&pixels, nullptr, nullptr);

                if (SUCCEEDED(result)) {

                    if (!channel.read(pixels, format.width, format.height)) {

                        std::memset(pixels, 16, format.width * format.height);
                        std::memset(pixels + format.width * format.height, 128, frame_bytes / 3);

                    }

                    result = buffer->Unlock();

                }

                if (SUCCEEDED(result))
                    result = buffer->SetCurrentLength(frame_bytes);

                if (SUCCEEDED(result))
                    result = sample->AddBuffer(buffer.Get());

                if (SUCCEEDED(result))
                    result = sample->SetSampleTime(MFGetSystemTime());

                if (SUCCEEDED(result))
                    result = sample->SetSampleDuration(frame_duration);

                if (SUCCEEDED(result))
                    result = sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);

                if (SUCCEEDED(result) && request)
                    result = sample->SetUnknown(MFSampleExtension_Token, request.Get());

                std::lock_guard lock(mutex);

                if (shutdown || generation != epoch)
                    continue;

                if (FAILED(result))
                    events->QueueEventParamVar(MEError, GUID_NULL, result, nullptr);
                else
                    events->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());

            }

        }

        public:
        HRESULT initialize(IMFMediaSource* source, const std::wstring& path) {

            parent = source;
            CAMERA_TRY(MFCreateEventQueue(&events));

            if (!path.empty() && !channel.open(path)) {

                const std::string stage(channel.open_stage);
                utils::report_activation(
                    std::wstring(stage.begin(), stage.end()) + L" failed with Windows error " +
                    std::to_wstring(channel.open_error) + L"; path=" + path
                );

                return HRESULT_FROM_WIN32(channel.open_error);

            }

            const auto preferred = channel.native_format().value_or(protocol::video_format{});
            CAMERA_TRY(configure_descriptor(preferred, channel.app_outputs_allowed()));
            selected = preferred;

            try {

                worker = std::jthread([this](std::stop_token token) {
                    deliver(token);
                });

            } catch (...) {

                return E_OUTOFMEMORY;

            }

            return S_OK;

        }

        ~camera_stream() {
            stop_worker();
        }

        void stop_worker() {

            {

                std::lock_guard lock(mutex);
                shutdown = true;
                generation++;
                requests.clear();
                parent.Reset();

                if (events)
                    events->Shutdown();

            }

            worker.request_stop();
            changed.notify_all();

            if (worker.joinable())
                worker.join();

        }

        HRESULT select_format(IMFMediaType* type) {

            std::lock_guard lock(mutex);
            const bool allow_apps = channel.app_outputs_allowed();
            const auto native = channel.native_format().value_or(selected);

            if (!allow_apps || app_formats != allow_apps)
                CAMERA_TRY(configure_descriptor(native, allow_apps));

            ComPtr<IMFMediaTypeHandler> handler;

            CAMERA_TRY(descriptor->GetMediaTypeHandler(&handler));
            CAMERA_TRY(handler->IsMediaTypeSupported(type, nullptr));

            UINT32 w = 0, h = 0, rate = 0, denominator = 0;
            CAMERA_TRY(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h));
            CAMERA_TRY(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &rate, &denominator));

            if (denominator != 1 || w < 2 || h < 2 || w > 3840 || h > 3840 || w % 2 || h % 2 ||
                static_cast<std::uint64_t>(w) * h > 3840ULL * 2160 || !rate || rate > 255)
                return MF_E_INVALIDMEDIATYPE;

            if (!allow_apps && (w != native.width || h != native.height || rate != native.fps))
                return MF_E_INVALIDMEDIATYPE;

            CAMERA_TRY(handler->SetCurrentMediaType(type));
            selected = {static_cast<std::uint16_t>(w), static_cast<std::uint16_t>(h), static_cast<std::uint16_t>(rate)};
            generation++;

            requests.clear();
            channel.request_format(selected, true);

            return S_OK;

        }

        HRESULT set_running(bool running, bool notify = true) {

            std::lock_guard lock(mutex);

            if (shutdown)
                return MF_E_SHUTDOWN;

            state = running ? MF_STREAM_STATE_RUNNING : MF_STREAM_STATE_STOPPED;
            generation++;
            requests.clear();
            changed.notify_all();
            PROPVARIANT time{};

            time.vt = VT_I8;
            time.hVal.QuadPart = MFGetSystemTime();

            return notify ? events->QueueEventParamVar(running ? MEStreamStarted : MEStreamStopped, GUID_NULL, S_OK, &time)
                          : S_OK;

        }

        STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override {

            ComPtr<IMFMediaEventQueue> queue;

            {

                std::lock_guard lock(mutex);

                if (shutdown)
                    return MF_E_SHUTDOWN;

                queue = events;

            }

            return queue->GetEvent(flags, event);

        }

        STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* value) override {
            std::lock_guard lock(mutex);

            return shutdown ? MF_E_SHUTDOWN : events->BeginGetEvent(callback, value);
        }

        STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
            std::lock_guard lock(mutex);

            return shutdown ? MF_E_SHUTDOWN : events->EndGetEvent(result, event);
        }

        STDMETHODIMP QueueEvent(MediaEventType type, REFGUID guid, HRESULT status, const PROPVARIANT* value) override {
            std::lock_guard lock(mutex);

            return shutdown ? MF_E_SHUTDOWN : events->QueueEventParamVar(type, guid, status, value);
        }

        STDMETHODIMP GetMediaSource(IMFMediaSource** source) override {
            std::lock_guard lock(mutex);

            return shutdown ? MF_E_SHUTDOWN : parent.CopyTo(source);
        }

        STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** stream) override {
            std::lock_guard lock(mutex);

            return shutdown ? MF_E_SHUTDOWN : descriptor.CopyTo(stream);
        }

        STDMETHODIMP RequestSample(IUnknown* request) override {

            std::lock_guard lock(mutex);

            if (shutdown)
                return MF_E_SHUTDOWN;

            if (state != MF_STREAM_STATE_RUNNING)
                return MF_E_INVALIDREQUEST;

            if (requests.size() >= 80)
                return MF_E_NOTACCEPTING;

            try {

                requests.emplace_back(request);

            } catch (...) {

                return E_OUTOFMEMORY;

            }

            changed.notify_all();

            return S_OK;

        }

        STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override {

            if (value == MF_STREAM_STATE_PAUSED)
                return MF_E_INVALID_STATE_TRANSITION;

            if (value != MF_STREAM_STATE_RUNNING && value != MF_STREAM_STATE_STOPPED)
                return E_INVALIDARG;

            return set_running(value == MF_STREAM_STATE_RUNNING, false);

        }

        STDMETHODIMP GetStreamState(MF_STREAM_STATE* value) override {

            if (!value)
                return E_POINTER;

            std::lock_guard lock(mutex);

            if (shutdown)
                return MF_E_SHUTDOWN;

            *value = state;

            return S_OK;

        }
    };

    class camera_source final :
        public RuntimeClass<
            RuntimeClassFlags<ClassicCom>,
            ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
            IMFSampleAllocatorControl,

            IKsControl,
            FtmBase>,
        public counted {

        private:
        std::mutex mutex;
        ComPtr<IMFMediaEventQueue> events;
        ComPtr<IMFAttributes> attributes;
        ComPtr<IMFPresentationDescriptor> descriptor;
        ComPtr<camera_stream> stream;

        bool shutdown = false;
        bool announced = false;

        public:
        HRESULT initialize(IMFAttributes* input) {

            CAMERA_TRY(MFCreateEventQueue(&events));
            CAMERA_TRY(MFCreateAttributes(&attributes, 4));

            if (input)
                CAMERA_TRY(input->CopyAllItems(attributes.Get()));

            std::wstring path;

            if (input) {

                wchar_t* text = nullptr;
                UINT32 length = 0;

                if (SUCCEEDED(input->GetAllocatedString(ipc_attribute, &text, &length))) {

                    path.assign(text, length);
                    CoTaskMemFree(text);

                }

            }

            stream = Microsoft::WRL::Make<camera_stream>();

            if (!stream)
                return E_OUTOFMEMORY;

            const HRESULT initialized = stream->initialize(this, path);

            if (FAILED(initialized)) {

                stream->stop_worker();
                stream.Reset();

                return initialized;

            }

            ComPtr<IMFStreamDescriptor> stream_descriptor;

            CAMERA_TRY(stream->GetStreamDescriptor(&stream_descriptor));
            IMFStreamDescriptor* streams[]{stream_descriptor.Get()};
            CAMERA_TRY(MFCreatePresentationDescriptor(1, streams, &descriptor));
            CAMERA_TRY(descriptor->SelectStream(0));
            ComPtr<IMFSensorProfileCollection> profiles;
            ComPtr<IMFSensorProfile> profile;

            CAMERA_TRY(MFCreateSensorProfileCollection(&profiles));
            CAMERA_TRY(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile));
            CAMERA_TRY(profile->AddProfileFilter(0, L"((RES==;FRT<=255,1;SUT==))"));
            CAMERA_TRY(profiles->AddProfile(profile.Get()));
            CAMERA_TRY(attributes->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.Get()));

            return S_OK;

        }

        ~camera_source() {
            if (stream)
                stream->stop_worker();
        }

        STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override {

            ComPtr<IMFMediaEventQueue> queue;

            {

                std::lock_guard lock(mutex);

                if (shutdown)
                    return MF_E_SHUTDOWN;

                queue = events;

            }

            return queue->GetEvent(flags, event);

        }

        STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* value) override {
            std::lock_guard lock(mutex);

            return shutdown ? MF_E_SHUTDOWN : events->BeginGetEvent(callback, value);
        }

        STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
            std::lock_guard lock(mutex);

            return shutdown ? MF_E_SHUTDOWN : events->EndGetEvent(result, event);
        }

        STDMETHODIMP QueueEvent(MediaEventType type, REFGUID guid, HRESULT status, const PROPVARIANT* value) override {
            std::lock_guard lock(mutex);

            return shutdown ? MF_E_SHUTDOWN : events->QueueEventParamVar(type, guid, status, value);
        }

        STDMETHODIMP GetCharacteristics(DWORD* flags) override {

            if (!flags)
                return E_POINTER;

            std::lock_guard lock(mutex);

            if (shutdown)
                return MF_E_SHUTDOWN;

            *flags = MFMEDIASOURCE_IS_LIVE;

            return S_OK;

        }

        STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** value) override {

            std::lock_guard lock(mutex);

            if (shutdown)
                return MF_E_SHUTDOWN;

            if (!value)
                return E_POINTER;

            ComPtr<IMFStreamDescriptor> current;

            CAMERA_TRY(stream->GetStreamDescriptor(&current));
            IMFStreamDescriptor* streams[]{current.Get()};
            CAMERA_TRY(MFCreatePresentationDescriptor(1, streams, value));

            return (*value)->SelectStream(0);

        }

        STDMETHODIMP Start(IMFPresentationDescriptor* value, const GUID* format, const PROPVARIANT* position) override {

            std::lock_guard lock(mutex);

            if (shutdown)
                return MF_E_SHUTDOWN;

            if (!value || !position)
                return E_INVALIDARG;

            if (format && *format != GUID_NULL)
                return MF_E_UNSUPPORTED_TIME_FORMAT;

            if (position->vt != VT_EMPTY && (position->vt != VT_I8 || position->hVal.QuadPart != 0))
                return MF_E_INVALIDREQUEST;

            DWORD count = 0;
            CAMERA_TRY(value->GetStreamDescriptorCount(&count));

            if (count != 1)
                return E_INVALIDARG;

            BOOL selected = FALSE;
            ComPtr<IMFStreamDescriptor> description;

            CAMERA_TRY(value->GetStreamDescriptorByIndex(0, &selected, &description));

            if (!selected)
                return MF_E_INVALIDREQUEST;

            DWORD identifier = 1;
            CAMERA_TRY(description->GetStreamIdentifier(&identifier));

            if (identifier != 0)
                return MF_E_INVALIDSTREAMNUMBER;

            ComPtr<IMFMediaTypeHandler> handler;
            ComPtr<IMFMediaType> type;

            CAMERA_TRY(description->GetMediaTypeHandler(&handler));
            CAMERA_TRY(handler->GetCurrentMediaType(&type));
            CAMERA_TRY(stream->select_format(type.Get()));
            CAMERA_TRY(events->QueueEventParamUnk(
                announced ? MEUpdatedStream : MENewStream,

                GUID_NULL,
                S_OK,
                static_cast<IMFMediaStream2*>(stream.Get())
            ));
            CAMERA_TRY(stream->set_running(true));

            announced = true;
            PROPVARIANT time{};
            time.vt = VT_I8;
            time.hVal.QuadPart = MFGetSystemTime();

            return events->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);

        }

        STDMETHODIMP Stop() override {

            std::lock_guard lock(mutex);

            if (shutdown)
                return MF_E_SHUTDOWN;

            CAMERA_TRY(stream->set_running(false));

            return events->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);

        }

        STDMETHODIMP Pause() override {
            return MF_E_INVALID_STATE_TRANSITION;
        }

        STDMETHODIMP Shutdown() override {

            ComPtr<camera_stream> previous;

            {

                std::lock_guard lock(mutex);

                if (shutdown)
                    return S_OK;

                shutdown = true;

                if (events)
                    events->Shutdown();

                previous = std::move(stream);
                descriptor.Reset();
                attributes.Reset();

            }

            if (previous)
                previous->stop_worker();

            return S_OK;

        }

        STDMETHODIMP GetSourceAttributes(IMFAttributes** value) override {
            std::lock_guard lock(mutex);

            return shutdown ? MF_E_SHUTDOWN : attributes.CopyTo(value);
        }

        STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** value) override {

            std::lock_guard lock(mutex);

            if (shutdown)
                return MF_E_SHUTDOWN;

            if (id != 0)
                return MF_E_INVALIDSTREAMNUMBER;

            ComPtr<IMFStreamDescriptor> description;

            CAMERA_TRY(stream->GetStreamDescriptor(&description));

            return description->QueryInterface(IID_PPV_ARGS(value));

        }

        STDMETHODIMP SetD3DManager(IUnknown*) override {
            return E_NOTIMPL;
        }

        STDMETHODIMP SetDefaultAllocator(DWORD id, IUnknown*) override {
            return id == 0 ? S_OK : MF_E_INVALIDSTREAMNUMBER;
        }

        STDMETHODIMP GetAllocatorUsage(DWORD id, DWORD* input, MFSampleAllocatorUsage* usage) override {

            if (!input || !usage)
                return E_POINTER;

            if (id != 0)
                return MF_E_INVALIDSTREAMNUMBER;

            *input = 0;
            *usage = MFSampleAllocatorUsage_UsesCustomAllocator;

            return S_OK;

        }

        STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* returned) override {
            if (returned)
                *returned = 0;

            return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
        }

        STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* returned) override {
            if (returned)
                *returned = 0;

            return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
        }

        STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* returned) override {
            if (returned)
                *returned = 0;

            return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
        }
    };

    class camera_activate final :
        public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>, FtmBase>,
        public counted {

        private:
        ComPtr<IMFAttributes> attributes;
        ComPtr<camera_source> source;

        std::mutex mutex;

        public:
        HRESULT initialize() {
            return MFCreateAttributes(&attributes, 4);
        }

        STDMETHODIMP ActivateObject(REFIID iid, void** value) override {

            if (!value)
                return E_POINTER;

            *value = nullptr;
            std::lock_guard lock(mutex);

            try {

                if (!source) {

                    auto candidate = Microsoft::WRL::Make<camera_source>();

                    if (!candidate)
                        return E_OUTOFMEMORY;

                    const HRESULT result = candidate->initialize(attributes.Get());

                    if (FAILED(result)) {

                        utils::report_activation(
                            L"Source initialization failed: " + std::to_wstring(static_cast<unsigned long>(result))
                        );
                        candidate->Shutdown();

                        return result;

                    }

                    source = candidate;

                }

                return source->QueryInterface(iid, value);

            } catch (...) {

                return E_OUTOFMEMORY;

            }

        }

        STDMETHODIMP ShutdownObject() override {

            std::lock_guard lock(mutex);

            if (source)
                source->Shutdown();

            source.Reset();

            return S_OK;

        }

        STDMETHODIMP DetachObject() override {
            std::lock_guard lock(mutex);
            source.Reset();

            return S_OK;
        }

        STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override {
            return attributes->GetItem(k, v);
        }

        STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* v) override {
            return attributes->GetItemType(k, v);
        }

        STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override {
            return attributes->CompareItem(k, v, r);
        }

        STDMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) override {
            return attributes->Compare(a, t, r);
        }

        STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override {
            return attributes->GetUINT32(k, v);
        }

        STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override {
            return attributes->GetUINT64(k, v);
        }

        STDMETHODIMP GetDouble(REFGUID k, double* v) override {
            return attributes->GetDouble(k, v);
        }

        STDMETHODIMP GetGUID(REFGUID k, GUID* v) override {
            return attributes->GetGUID(k, v);
        }

        STDMETHODIMP GetStringLength(REFGUID k, UINT32* v) override {
            return attributes->GetStringLength(k, v);
        }

        STDMETHODIMP GetString(REFGUID k, LPWSTR v, UINT32 n, UINT32* l) override {
            return attributes->GetString(k, v, n, l);
        }

        STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* v, UINT32* n) override {
            return attributes->GetAllocatedString(k, v, n);
        }

        STDMETHODIMP GetBlobSize(REFGUID k, UINT32* v) override {
            return attributes->GetBlobSize(k, v);
        }

        STDMETHODIMP GetBlob(REFGUID k, UINT8* v, UINT32 n, UINT32* l) override {
            return attributes->GetBlob(k, v, n, l);
        }

        STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** v, UINT32* n) override {
            return attributes->GetAllocatedBlob(k, v, n);
        }

        STDMETHODIMP GetUnknown(REFGUID k, REFIID i, void** v) override {
            return attributes->GetUnknown(k, i, v);
        }

        STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override {
            return attributes->SetItem(k, v);
        }

        STDMETHODIMP DeleteItem(REFGUID k) override {
            return attributes->DeleteItem(k);
        }

        STDMETHODIMP DeleteAllItems() override {
            return attributes->DeleteAllItems();
        }

        STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override {
            return attributes->SetUINT32(k, v);
        }

        STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override {
            return attributes->SetUINT64(k, v);
        }

        STDMETHODIMP SetDouble(REFGUID k, double v) override {
            return attributes->SetDouble(k, v);
        }

        STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override {
            return attributes->SetGUID(k, v);
        }

        STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override {
            return attributes->SetString(k, v);
        }

        STDMETHODIMP SetBlob(REFGUID k, const UINT8* v, UINT32 n) override {
            return attributes->SetBlob(k, v, n);
        }

        STDMETHODIMP SetUnknown(REFGUID k, IUnknown* v) override {
            return attributes->SetUnknown(k, v);
        }

        STDMETHODIMP LockStore() override {
            return attributes->LockStore();
        }

        STDMETHODIMP UnlockStore() override {
            return attributes->UnlockStore();
        }

        STDMETHODIMP GetCount(UINT32* n) override {
            return attributes->GetCount(n);
        }

        STDMETHODIMP GetItemByIndex(UINT32 n, GUID* k, PROPVARIANT* v) override {
            return attributes->GetItemByIndex(n, k, v);
        }

        STDMETHODIMP CopyAllItems(IMFAttributes* v) override {
            return attributes->CopyAllItems(v);
        }
    };

    class camera_factory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory>, public counted {

        public:
        STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** value) override {

            if (!value)
                return E_POINTER;

            *value = nullptr;

            if (outer)
                return CLASS_E_NOAGGREGATION;

            auto activation = Microsoft::WRL::Make<camera_activate>();

            if (!activation)
                return E_OUTOFMEMORY;

            CAMERA_TRY(activation->initialize());

            return activation->QueryInterface(iid, value);

        }

        STDMETHODIMP LockServer(BOOL lock) override {
            locks += lock ? 1 : -1;

            return S_OK;
        }

    };

};

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        camdroid::webcam::module = instance;

    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** value) {

    if (!value)
        return E_POINTER;

    *value = nullptr;

    if (clsid != camdroid::webcam::source_id)
        return CLASS_E_CLASSNOTAVAILABLE;

    auto factory = Microsoft::WRL::Make<camdroid::webcam::camera_factory>();

    return factory ? factory->QueryInterface(iid, value) : E_OUTOFMEMORY;

}

STDAPI DllCanUnloadNow() {
    return camdroid::webcam::objects == 0 && camdroid::webcam::locks == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {

    wchar_t path[32768]{};

    if (!GetModuleFileNameW(camdroid::webcam::module, path, 32768))
        return HRESULT_FROM_WIN32(GetLastError());

    const std::wstring key =
        std::wstring(L"Software\\Classes\\CLSID\\") + camdroid::webcam::source_clsid + L"\\InprocServer32";
    HKEY registration = nullptr;
    LSTATUS result = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,

        key.c_str(),
        0,
        nullptr,
        0,
        KEY_WRITE | KEY_WOW64_64KEY,

        nullptr,
        &registration,
        nullptr
    );

    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    result = RegSetValueExW(
        registration,
        nullptr,
        0,
        REG_SZ,

        reinterpret_cast<const BYTE*>(path),
        static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t))
    );

    if (result == ERROR_SUCCESS)
        result = RegSetValueExW(registration, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(L"Both"), 10);

    RegCloseKey(registration);

    return HRESULT_FROM_WIN32(result);

}

STDAPI DllUnregisterServer() {
    const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + camdroid::webcam::source_clsid;
    const auto result = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());

    return result == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(result);
}