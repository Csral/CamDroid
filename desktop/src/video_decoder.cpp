#include "video_decoder.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>

#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>

#include <stdexcept>
#include <utility>
#include <future>
#include <stop_token>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>

#include <libavutil/pixdesc.h>
}

namespace camdroid {

    namespace utils {

        class codec_failure : public std::runtime_error {

            public:
            explicit codec_failure(const std::string& message) : std::runtime_error(message) {}

        };

        static void check_decoder(int result, const char* operation) {

            if (result >= 0)
                return;

            if (result == AVERROR(ENOMEM))
                throw std::bad_alloc();

            char message[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(result, message, sizeof(message));
            throw codec_failure(std::string(operation) + ": " + message);

        }

        static bool is_annex_b(const std::vector<std::uint8_t>& data) {
            if (data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
                return true;

            return data.size() >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1;
        }

        struct decoder_candidate {

            decoder_option option;
            const AVCodec* codec = nullptr;
            AVHWDeviceType device = AV_HWDEVICE_TYPE_NONE;
            AVPixelFormat format = AV_PIX_FMT_NONE;
            bool device_context = false;

            bool proven = false;
            bool failed = false;
            bool attempted = false;
            bool low_latency = false;

        };

        struct hardware_device {

            AVBufferRef* value = nullptr;

            ~hardware_device() {
                av_buffer_unref(&value);
            }

        };

        struct decoder_session {

            AVCodecContext* context = nullptr;
            AVFrame* frame = nullptr;
            AVFrame* software = nullptr;
            AVPacket* packet = nullptr;
            AVPixelFormat target = AV_PIX_FMT_NONE;

            ~decoder_session() {

                av_packet_free(&packet);
                av_frame_free(&frame);
                av_frame_free(&software);
                avcodec_free_context(&context);

            }

            static AVPixelFormat select_format(AVCodecContext* context, const AVPixelFormat* formats) {

                const auto* session = static_cast<decoder_session*>(context->opaque);

                for (const auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {

                    if (session->target != AV_PIX_FMT_NONE) {

                        if (*format == session->target)
                            return *format;

                    } else {

                        const auto* description = av_pix_fmt_desc_get(*format);
                        if (description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL))
                            return *format;

                    }

                }

                // A hardware trial must never silently become a software trial.
                return AV_PIX_FMT_NONE;

            }

            int receive(const video_decoder::frame_handler& consume) {

                while (true) {

                    const int result = avcodec_receive_frame(context, frame);
                    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                        return result;

                    check_decoder(result, "Receiving decoded frame");

                    try {

                        const auto* description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
                        AVFrame* output = frame;

                        if (description && (description->flags & AV_PIX_FMT_FLAG_HWACCEL)) {

                            check_decoder(av_hwframe_transfer_data(software, frame, 0), "Downloading decoded frame");
                            check_decoder(av_frame_copy_props(software, frame), "Copying decoded frame properties");
                            output = software;

                        }

                        consume(*output);

                    } catch (...) {

                        av_frame_unref(software);
                        av_frame_unref(frame);
                        throw;

                    }

                    av_frame_unref(software);
                    av_frame_unref(frame);

                }

            }

            void send(const protocol::video_frame& footage, bool keyframe, const video_decoder::frame_handler& consume) {

                av_packet_unref(packet);
                check_decoder(av_new_packet(packet, static_cast<int>(footage.data.size())), "Allocating encoded packet");
                std::memcpy(packet->data, footage.data.data(), footage.data.size());
                packet->pts = static_cast<std::int64_t>(footage.pts_us);
                packet->dts = AV_NOPTS_VALUE;

                if (keyframe)
                    packet->flags |= AV_PKT_FLAG_KEY;

                try {

                    int result = avcodec_send_packet(context, packet);

                    if (result == AVERROR(EAGAIN)) {

                        receive(consume);
                        result = avcodec_send_packet(context, packet);

                    }

                    check_decoder(result, "Submitting encoded frame");
                    receive(consume);

                } catch (...) {

                    av_packet_unref(packet);
                    throw;

                }

                av_packet_unref(packet);

            }

            void finish(const video_decoder::frame_handler& consume) {

                int result = avcodec_send_packet(context, nullptr);

                if (result == AVERROR(EAGAIN)) {

                    receive(consume);
                    result = avcodec_send_packet(context, nullptr);

                }

                if (result == AVERROR_EOF)
                    return;

                check_decoder(result, "Starting decoder drain");
                if (receive(consume) != AVERROR_EOF)
                    throw codec_failure("Decoder drain did not reach end of stream.");

            }

        };

        static std::vector<decoder_candidate> decoder_candidates(AVCodecID id) {

            std::vector<decoder_candidate> result;
            std::set<std::string> identifiers;
            void* iterator = nullptr;

            const auto append =
                [&](const AVCodec* codec, const AVCodecHWConfig* hardware, bool internal, bool low_latency = false) {

                    decoder_candidate candidate;
                    candidate.codec = codec;
                    candidate.low_latency = low_latency;
                    candidate.option.hardware = hardware || internal;
                    candidate.option.specialized = candidate.option.hardware && codec->wrapper_name;
                    candidate.option.decode_ms = std::numeric_limits<double>::infinity();

                    if (hardware) {

                        candidate.device = hardware->device_type;
                        candidate.format = hardware->pix_fmt;
                        candidate.device_context = (hardware->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0;

                    }

                    const auto* device = av_hwdevice_get_type_name(candidate.device);
                    const std::string backend =
                        device ? device : (internal ? "hardware" : (low_latency ? "software-low-latency" : "software"));
                    candidate.option.id = std::string(codec->name) + ":" + backend;
                    candidate.option.name = std::string(codec->name) + " / " + backend;

                    if (identifiers.insert(candidate.option.id).second)
                        result.push_back(std::move(candidate));

                };

            while (const auto* codec = av_codec_iterate(&iterator)) {

                if (codec->id != id || !av_codec_is_decoder(codec))
                    continue;

                bool hardware = false;

                for (int index = 0; const auto* config = avcodec_get_hw_config(codec, index); ++index) {

                    if (!(config->methods & (AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX | AV_CODEC_HW_CONFIG_METHOD_INTERNAL)))
                        continue;

                    append(codec, config, false);
                    hardware = true;

                }

                if (codec->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID)) {

                    if (!hardware)
                        append(codec, nullptr, true);

                } else {

                    append(codec, nullptr, false);

                    if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
                        append(codec, nullptr, false, true);

                }

            }

            // Discovery order favours codec-specific hardware. Measured timings
            // decide the winner, without an OS or GPU vendor preference table.
            std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {

                const int a = left.option.hardware ? (left.option.specialized ? 0 : 1) : 2;
                const int b = right.option.hardware ? (right.option.specialized ? 0 : 1) : 2;
                if (a != b)
                    return a < b;

                return left.low_latency && !right.low_latency;

            });

            return result;

        }

        struct buffered_frame {

            std::shared_ptr<const protocol::video_frame> footage;
            bool keyframe = false;

        };

    };

    struct video_decoder::state {

        frame_handler on_frame;
        selection_handler on_selection;
        cancellation_handler cancelled;
        std::optional<protocol::video_config> configuration;
        std::vector<utils::decoder_candidate> candidates;

        std::unique_ptr<utils::decoder_session> session;
        std::size_t active = 0;
        int width = 0;
        int height = 0;
        int profile = AV_PROFILE_UNKNOWN;

        int level = AV_LEVEL_UNKNOWN;
        std::map<AVHWDeviceType, std::unique_ptr<utils::hardware_device>> devices;
        std::set<AVHWDeviceType> unavailable_devices;
        std::string preferred = "auto";

        bool pending_selection = false;
        bool automatic = false;
        bool initial_identified = false;
        bool select_after_scan = false;
        bool announce_best = false;

        std::string identified_choice;
        bool scan_requested = true;
        std::chrono::steady_clock::time_point next_trial{};

        std::vector<utils::buffered_frame> sample;
        std::vector<utils::buffered_frame> replay;
        std::size_t sample_bytes = 0;
        std::size_t replay_bytes = 0;

        bool replay_valid = false;
        bool sample_full = false;
        bool measured = false;
        bool awaiting_keyframe = true;
        bool ended = false;
        std::optional<std::int64_t> delivered_pts;

        struct benchmark_result {

            std::vector<utils::decoder_candidate> candidates;
            std::uint64_t generation = 0;
            bool complete = false;

        };

        std::future<benchmark_result> benchmark;
        std::stop_source benchmark_stop;
        std::uint64_t benchmark_generation = 0;

        ~state() {
            benchmark_stop.request_stop();
        }

        void cancel_benchmark() {
            benchmark_stop.request_stop();
            ++benchmark_generation;
        }

        void collect_benchmark() {

            if (!benchmark.valid() || benchmark.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;

            auto result = benchmark.get();

            if (result.generation != benchmark_generation)
                return;

            for (std::size_t index = 0; index < candidates.size(); ++index) {

                auto& live = candidates[index];
                const auto& trial = result.candidates[index];

                if (live.failed || !trial.attempted)
                    continue;

                live.attempted = true;

                if (trial.proven && !trial.failed) {

                    live.option.decode_ms = trial.option.decode_ms;
                    live.option.buffered_frames = trial.option.buffered_frames;
                    live.proven = true;

                } else if (index != active) {

                    live.failed = true;

                }

            }

            measured = result.complete;
            scan_requested = !result.complete;
            initial_identified = result.complete;
            next_trial = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            const auto ranked = order(false);

            if (automatic || (select_after_scan && result.complete)) {

                if (!ranked.empty() && select_after_scan && result.complete) {

                    identified_choice = candidates[ranked.front()].option.id;
                    announce_best = true;
                    select_after_scan = false;

                }

                pending_selection = !ranked.empty() && ranked.front() != active;

                if (announce_best && !pending_selection) {

                    announce_best = false;
                    preferred = candidates[active].option.id;
                    identified_choice.clear();
                    notify("Selected best for you");
                    return;

                }

            }

            notify(
                pending_selection ? "Decoder comparison complete; switching at the next keyframe."
                                  : "Decoder recommendations updated; using " + candidates[active].option.name
            );

        }

        void measure() {

            collect_benchmark();

            if (sample.empty() || measured || stopping() || benchmark.valid())
                return;

            auto trial = std::make_unique<state>();
            trial->configuration = configuration;
            trial->candidates = candidates;
            trial->active = active;
            trial->width = width ? width : session ? session->context->width : 0;
            trial->height = height ? height : session ? session->context->height : 0;

            trial->profile = session ? session->context->profile : profile;
            trial->level = session ? session->context->level : level;
            trial->sample = std::move(sample);
            sample_bytes = 0;
            sample_full = false;

            benchmark_stop = std::stop_source{};
            const auto stop = benchmark_stop.get_token();

            trial->cancelled = [stop] {
                return stop.stop_requested();
            };

            const auto generation = benchmark_generation;
            measured = true;

            benchmark = std::async(std::launch::async, [trial = std::move(trial), generation]() mutable {
                auto owned = std::move(trial);
                owned->measure_sample();

                benchmark_result result{std::move(owned->candidates), generation, owned->measured};
                owned.reset();

                return result;
            });

            notify("Comparing available decoders in the background...");

        }

        bool stopping() const {
            return cancelled && cancelled();
        }

        void notify(const std::string& message) {

            if (!on_selection)
                return;

            decoder_selection selection;
            selection.requested = preferred;
            selection.message = message;
            selection.identifying = benchmark.valid() || scan_requested;
            selection.automatic = automatic;

            for (const auto index : order(false)) {

                if (candidates[index].proven && !candidates[index].failed && selection.recommended.size() < 3)
                    selection.recommended.push_back(candidates[index].option.id);

            }

            if (session && active < candidates.size())
                selection.active = candidates[active].option.id;

            for (const auto& candidate : candidates) {

                if (candidate.proven && !candidate.failed)
                    selection.options.push_back(candidate.option);

            }

            on_selection(selection);

        }

        std::unique_ptr<utils::decoder_session> open(std::size_t index) {

            const auto& candidate = candidates.at(index);
            auto next = std::make_unique<utils::decoder_session>();
            next->context = avcodec_alloc_context3(candidate.codec);
            next->frame = av_frame_alloc();
            next->software = av_frame_alloc();

            next->packet = av_packet_alloc();
            if (!next->context || !next->frame || !next->software || !next->packet)
                throw std::bad_alloc();

            next->target = candidate.format;
            next->context->opaque = next.get();
            next->context->get_format = utils::decoder_session::select_format;
            next->context->pkt_timebase = AVRational{1, 1'000'000};
            next->context->width = width;

            next->context->height = height;
            next->context->profile = profile;
            next->context->level = level;

            if (configuration->settings.fps)
                next->context->framerate = AVRational{configuration->settings.fps, 1};

            // Show the baseline without a frame queue, but also compare automatic
            // frame/slice threading for higher software decoding throughput.
            next->context->thread_count = candidate.option.hardware ? 1 : 0;
            next->context->thread_type =
                candidate.option.hardware ? 0 : (candidate.low_latency ? FF_THREAD_SLICE : FF_THREAD_FRAME | FF_THREAD_SLICE);

            if (candidate.device_context) {

                if (unavailable_devices.contains(candidate.device))
                    throw utils::codec_failure("Hardware device is unavailable.");

                auto& device = devices[candidate.device];

                if (!device) {

                    auto created = std::make_unique<utils::hardware_device>();
                    const int result = av_hwdevice_ctx_create(&created->value, candidate.device, nullptr, nullptr, 0);

                    if (result < 0) {

                        unavailable_devices.insert(candidate.device);
                        utils::check_decoder(result, "Creating hardware decoder device");

                    }

                    device = std::move(created);

                }

                next->context->hw_device_ctx = av_buffer_ref(device->value);
                if (!next->context->hw_device_ctx)
                    throw std::bad_alloc();

            }

            const auto& config = *configuration;
            const auto size = config.csd0.size() + config.csd1.size();
            next->context->extradata = static_cast<std::uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!next->context->extradata)
                throw std::bad_alloc();

            next->context->extradata_size = static_cast<int>(size);
            if (!config.csd0.empty())
                std::memcpy(next->context->extradata, config.csd0.data(), config.csd0.size());

            if (!config.csd1.empty())
                std::memcpy(next->context->extradata + config.csd0.size(), config.csd1.data(), config.csd1.size());

            utils::check_decoder(avcodec_open2(next->context, candidate.codec, nullptr), "Opening video decoder");
            return next;

        }

        void deliver(const AVFrame& source) {

            auto& frame = const_cast<AVFrame&>(source);
            width = frame.width;
            height = frame.height;
            const auto pts = frame.pts != AV_NOPTS_VALUE ? frame.pts : frame.best_effort_timestamp;

            // Rebuild references without presenting already displayed frames again.
            if (delivered_pts && pts != AV_NOPTS_VALUE && pts <= *delivered_pts)
                return;

            const auto rotation = std::to_string(configuration->rotation);
            if (av_dict_set(&frame.metadata, "camdroid.rotation", rotation.c_str(), 0) < 0 ||
                av_dict_set(&frame.metadata, "camdroid.mirrored", configuration->mirrored ? "1" : "0", 0) < 0)
                throw std::bad_alloc();

            on_frame(frame);
            if (pts != AV_NOPTS_VALUE)
                delivered_pts = pts;

        }

        std::vector<std::size_t> order(bool honour_preference = true) const {

            std::vector<std::size_t> indices;

            for (std::size_t index = 0; index < candidates.size(); ++index) {

                if (!candidates[index].failed)
                    indices.push_back(index);

            }

            const double budget = 1000.0 / (configuration->settings.fps ? configuration->settings.fps : 30);

            const auto score = [&](std::size_t index, bool realtime) {

                const auto& candidate = candidates[index];
                // Close results favour specialized hardware. Frame queues count
                // against latency once both options can sustain the capture rate.
                const double bias = candidate.option.specialized ? 0.98 : (candidate.option.hardware ? 0.99 : 1.0);
                const double delay = realtime ? candidate.option.buffered_frames * budget : 0;
                return candidate.option.decode_ms * bias + delay;

            };

            std::stable_sort(indices.begin(), indices.end(), [&](auto left, auto right) {

                const auto& a = candidates[left];
                const auto& b = candidates[right];
                if (honour_preference && !identified_choice.empty() &&
                    (a.option.id == identified_choice) != (b.option.id == identified_choice))
                    return a.option.id == identified_choice;

                if (honour_preference && !automatic && (a.option.id == preferred) != (b.option.id == preferred))
                    return a.option.id == preferred;

                const bool fast_a = a.proven && a.option.decode_ms <= budget;
                const bool fast_b = b.proven && b.option.decode_ms <= budget;
                if (fast_a != fast_b)
                    return fast_a;

                return score(left, fast_a) < score(right, fast_b);

            });

            return indices;

        }

        bool activate(bool rebuild) {

            std::string last_error = "No working decoder is available for this stream.";
            // Switch only with a replay interval or at a keyframe, so the old
            // surfaces can be released before opening a replacement session.
            session.reset();

            for (const auto index : order()) {

                if (stopping())
                    return false;

                try {

                    auto next = open(index);

                    if (rebuild && replay_valid) {

                        for (const auto& buffered : replay) {

                            if (stopping())
                                return false;

                            next->send(*buffered.footage, buffered.keyframe, [&](const AVFrame& frame) {
                                deliver(frame);
                            });

                        }

                    }

                    session = std::move(next);
                    active = index;
                    pending_selection = false;
                    awaiting_keyframe = !rebuild || !replay_valid;
                    const bool fallback = announce_best ? candidates[index].option.id != identified_choice
                                                        : preferred != "auto" && candidates[index].option.id != preferred;

                    if (announce_best && !fallback) {

                        announce_best = false;
                        preferred = candidates[index].option.id;
                        identified_choice.clear();
                        notify("Selected best for you");

                    } else {

                        announce_best = false;
                        notify(
                            (fallback ? "Requested decoder unavailable; using " : "Using ") + candidates[index].option.name
                        );

                    }

                    return true;

                } catch (const utils::codec_failure& error) {

                    candidates[index].failed = true;
                    last_error = error.what();

                }

            }

            throw utils::codec_failure(last_error);

        }

        void measure_sample() {

            if (sample.empty() || measured || stopping())
                return;

            measured = true;
            notify("Comparing available decoders on the current stream...");

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            std::vector<std::size_t> trials;
            // Always measure the live baseline before spending time on drivers.
            if (!candidates[active].attempted && !candidates[active].failed)
                trials.push_back(active);

            for (std::size_t index = 0; index < candidates.size(); ++index) {

                if (index != active && !candidates[index].failed && !candidates[index].attempted)
                    trials.push_back(index);

            }

            for (const auto index : trials) {

                if (stopping() || std::chrono::steady_clock::now() >= deadline)
                    break;

                auto& candidate = candidates[index];
                candidate.attempted = true;

                try {

                    auto trial = open(index);
                    std::size_t frames = 0;

                    const auto count = [&](const AVFrame& frame) {
                        if (frame.flags & AV_FRAME_FLAG_CORRUPT)
                            throw utils::codec_failure("Decoder produced a corrupt trial frame.");

                        ++frames;
                    };

                    const auto start = std::chrono::steady_clock::now();

                    for (const auto& buffered : sample) {

                        if (stopping())
                            return;

                        trial->send(*buffered.footage, buffered.keyframe, count);

                    }

                    trial->finish(count);
                    if (frames != sample.size())
                        throw utils::codec_failure("Decoder did not produce the complete sample.");

                    const double elapsed =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    candidate.option.decode_ms = elapsed / frames;
                    const auto* context = trial->context;
                    const int thread_delay = (context->active_thread_type & FF_THREAD_FRAME) ? context->thread_count - 1 : 0;

                    candidate.option.buffered_frames = std::max({0, context->delay, thread_delay});
                    candidate.proven = true;

                } catch (const utils::codec_failure&) {

                    // A second decoder instance can exceed a device's session
                    // limit even while its current live instance works correctly.
                    if (index != active || !candidate.proven || !session)
                        candidate.failed = true;

                }

            }

            sample.clear();
            sample_bytes = 0;
            sample_full = false;

            measured = std::none_of(candidates.begin(), candidates.end(), [](const auto& candidate) {
                return !candidate.attempted && !candidate.failed;
            });

        }

        void remember(const protocol::video_frame& footage, bool keyframe) {

            constexpr std::size_t max_replay = 64 * 1024 * 1024;
            constexpr std::size_t max_sample = 16 * 1024 * 1024;
            std::shared_ptr<const protocol::video_frame> saved;

            const auto buffered = [&] {
                if (!saved)
                    saved = std::make_shared<protocol::video_frame>(footage);

                return utils::buffered_frame{saved, keyframe};
            };

            if (keyframe) {

                replay.clear();
                replay_bytes = 0;
                replay_valid = true;

            }

            if (replay_valid) {

                if (footage.data.size() > max_replay - replay_bytes || replay.size() >= 300) {

                    replay.clear();
                    replay_bytes = 0;
                    replay_valid = false;

                } else {

                    replay.push_back(buffered());
                    replay_bytes += footage.data.size();

                }

            }

            if (!measured && (keyframe || !sample.empty())) {

                if (footage.data.size() <= max_sample - sample_bytes) {

                    sample.push_back(buffered());
                    sample_bytes += footage.data.size();

                } else if (!sample.empty()) {

                    sample_full = true;

                } else {

                    measured = true;
                    notify("Keeping the software decoder; the trial sample exceeds the memory limit.");

                }

            }

        }

    };

    video_decoder::video_decoder(frame_handler handler, selection_handler selection, cancellation_handler cancelled)
        : engine(std::make_unique<state>()) {

        if (!handler)
            throw std::invalid_argument("Video decoder failure: Missing frame handler.");

        engine->on_frame = std::move(handler);
        engine->on_selection = std::move(selection);
        engine->cancelled = std::move(cancelled);

    }

    video_decoder::~video_decoder() = default;

    void video_decoder::configure(const protocol::video_config& config) {

        if (config.codec != protocol::VideoCodec::H264)
            throw std::invalid_argument("Video decoder failure: Unsupported codec.");

        if (config.csd0.empty() && config.csd1.empty())
            throw std::invalid_argument("Video decoder failure: Missing codec configuration.");

        if ((!config.csd0.empty() && !utils::is_annex_b(config.csd0)) ||
            (!config.csd1.empty() && !utils::is_annex_b(config.csd1)))
            throw std::invalid_argument("Video decoder failure: Expected Annex B codec configuration.");

        const auto limit = static_cast<std::size_t>(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE);
        if (config.csd0.size() > limit || config.csd1.size() > limit - config.csd0.size())
            throw std::length_error("Video decoder failure: Codec configuration is too large.");

        drain();
        auto& state = *engine;
        state.cancel_benchmark();
        state.session.reset();
        state.devices.clear();
        state.configuration = config;

        state.width = state.height = 0;
        state.profile = AV_PROFILE_UNKNOWN;
        state.level = AV_LEVEL_UNKNOWN;
        state.candidates = utils::decoder_candidates(AV_CODEC_ID_H264);

        state.unavailable_devices.clear();
        state.sample.clear();
        state.replay.clear();
        state.sample_bytes = state.replay_bytes = 0;
        state.sample_full = state.replay_valid = false;

        state.scan_requested = !state.initial_identified || state.automatic;
        state.measured = !state.scan_requested;

        state.awaiting_keyframe = true;
        state.ended = false;
        state.pending_selection = state.preferred != "auto";
        state.delivered_pts.reset();
        // Display software-decoded frames immediately while collecting the sample.
        for (std::size_t index = 0; index < state.candidates.size(); ++index) {

            if (state.candidates[index].option.hardware)
                continue;

            try {

                state.session = state.open(index);
                state.active = index;
                state.notify(
                    state.scan_requested ? "Collecting a decoder trial sample."
                                         : "Using " + state.candidates[index].option.name
                );

                return;

            } catch (const utils::codec_failure&) {

                state.candidates[index].failed = true;

            }

        }

        state.activate(false);

    }

    void video_decoder::set_preference(const std::string& id) {

        const auto requested = id.empty() ? std::string("auto") : id;
        if (engine->preferred == requested)
            return;

        engine->preferred = requested;
        engine->select_after_scan = false;
        engine->announce_best = false;
        engine->identified_choice.clear();
        engine->pending_selection = true;

        for (auto& candidate : engine->candidates) {

            if (candidate.option.id == requested)
                candidate.failed = false;

        }

        engine->notify("Decoder change queued for the next keyframe.");

    }

    void video_decoder::identify_best(bool select_best) {

        engine->select_after_scan = select_best;
        engine->announce_best = false;
        engine->identified_choice.clear();
        engine->cancel_benchmark();
        engine->sample.clear();
        engine->sample_bytes = 0;

        engine->sample_full = false;
        engine->measured = false;
        engine->scan_requested = true;

        for (auto& candidate : engine->candidates) {

            candidate.attempted = false;
            candidate.failed = false;

        }

        engine->notify("Identify best: waiting for a keyframe sample.");

    }

    void video_decoder::set_automatic(bool enabled) {

        if (engine->automatic == enabled)
            return;

        engine->automatic = enabled;

        if (enabled) {

            identify_best(false);

        } else {

            engine->cancel_benchmark();
            engine->scan_requested = false;
            engine->measured = true;
            engine->pending_selection = false;

            if (engine->session)
                engine->preferred = engine->candidates[engine->active].option.id;

            engine->notify("Automatic decoder switching disabled.");

        }

    }

    void video_decoder::update_settings(protocol::video_settings settings) {

        if (!engine->configuration)
            return;

        const auto previous = engine->configuration->settings;
        engine->configuration->settings = settings;

        if (engine->ended || (previous.fps == settings.fps && previous.bitrate == settings.bitrate))
            return;

        if (engine->automatic)
            identify_best(false);

    }

    void video_decoder::discontinuity() {

        auto& state = *engine;

        if (state.session)
            avcodec_flush_buffers(state.session->context);

        state.awaiting_keyframe = true;
        state.sample.clear();
        state.replay.clear();
        state.sample_bytes = state.replay_bytes = 0;
        state.sample_full = false;
        state.replay_valid = false;

    }

    bool video_decoder::decode(const protocol::video_frame& footage, bool keyframe) {

        auto& state = *engine;
        state.collect_benchmark();

        if (state.automatic && !state.benchmark.valid() && state.measured &&
            std::chrono::steady_clock::now() >= state.next_trial)
            identify_best(false);

        if (!state.session || state.ended || state.stopping() || (state.awaiting_keyframe && !keyframe))
            return false;

        if (footage.data.empty() || !utils::is_annex_b(footage.data))
            throw std::invalid_argument("Video decoder failure: Expected a nonempty Annex B frame.");

        const auto limit = static_cast<std::size_t>(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE);
        if (footage.data.size() > limit ||
            footage.pts_us > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::length_error("Video decoder failure: Frame size or timestamp exceeds decoder limits.");

        state.remember(footage, keyframe);

        if (keyframe && state.pending_selection && state.width > 0) {

            state.pending_selection = false;
            if (!state.activate(false))
                return false;

        }

        try {

            const auto start = std::chrono::steady_clock::now();
            std::chrono::steady_clock::duration callbacks{};
            std::size_t frames = 0;

            state.session->send(footage, keyframe, [&](const AVFrame& frame) {

                const auto callback_start = std::chrono::steady_clock::now();
                state.deliver(frame);
                callbacks += std::chrono::steady_clock::now() - callback_start;
                ++frames;

            });

            if (frames) {

                auto& candidate = state.candidates[state.active];
                candidate.proven = true;

                if (!candidate.attempted || !std::isfinite(candidate.option.decode_ms)) {

                    const double elapsed =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start - callbacks)
                            .count();
                    candidate.option.decode_ms = elapsed / frames;

                }

            }

            state.awaiting_keyframe = false;

        } catch (const utils::codec_failure& error) {

            state.candidates[state.active].failed = true;
            state.session.reset();
            const auto device = state.candidates[state.active].device;
            state.devices.erase(device);
            state.unavailable_devices.erase(device);

            state.notify(std::string("Decoder failed; trying another option: ") + error.what());
            state.activate(true);

        }

        if (!state.measured && !state.sample.empty()) {

            const auto span = state.sample.back().footage->pts_us - state.sample.front().footage->pts_us;
            const double budget = 1000.0 / (state.configuration->settings.fps ? state.configuration->settings.fps : 30);
            const bool overloaded =
                state.candidates[state.active].proven && state.candidates[state.active].option.decode_ms > budget;

            if (state.sample_full || state.sample.size() >= 8 || overloaded || (state.sample.size() >= 2 && span >= 250000))
                state.measure();

        }

        return true;

    }

    void video_decoder::drain() {

        auto& state = *engine;
        if (!state.session || state.ended || state.stopping())
            return;

        state.cancel_benchmark();

        while (!state.stopping()) {

            try {

                state.session->finish([&](const AVFrame& frame) {
                    state.deliver(frame);
                });

                state.ended = true;
                state.replay.clear();
                state.replay_bytes = 0;
                state.replay_valid = false;
                return;

            } catch (const utils::codec_failure&) {

                state.candidates[state.active].failed = true;
                state.session.reset();
                state.activate(true);

            }

        }

    }

}