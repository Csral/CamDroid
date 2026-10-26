#include "application.h"
#include "tcp_socket.h"
#include "session.h"
#include "video_decoder.h"
#include "video_receiver.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>

#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>
#include <cstring>

#include <map>
#include <iterator>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/dict.h>
}

#ifndef _WIN32
#include <fcntl.h>
#include <sys/select.h>
#include <cerrno>
#endif

namespace camdroid {

    namespace utils {

        class receiver_discovery {

            private:
            #ifdef _WIN32
            SOCKET socket = INVALID_SOCKET;
            #else
            int socket = -1;
            #endif

            public:
            receiver_discovery() {
                socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            }

            receiver_discovery(const receiver_discovery&) = delete;
            receiver_discovery& operator=(const receiver_discovery&) = delete;

            ~receiver_discovery() {

                #ifdef _WIN32
                if (socket != INVALID_SOCKET)
                    ::closesocket(socket);

                #else
                if (socket >= 0)
                    ::close(socket);

                #endif

            }

            void run(std::stop_token token, std::uint16_t port, const std::atomic<bool>& busy) {

                #ifdef _WIN32
                if (socket == INVALID_SOCKET)
                    throw std::runtime_error("Discovery socket creation failed.");

                #else
                if (socket < 0 || socket >= FD_SETSIZE)
                    throw std::runtime_error("Discovery socket creation failed.");

                #endif
                const int reuse = 1;

                if (::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0)
                    throw std::runtime_error("Cannot configure discovery socket.");

                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_port = htons(39696);
                address.sin_addr.s_addr = htonl(INADDR_ANY);

                if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
                    throw std::runtime_error("Cannot listen for desktop discovery on UDP port 39696.");

                #ifdef _WIN32
                u_long mode = 1;

                if (::ioctlsocket(socket, FIONBIO, &mode) != 0)
                    throw std::runtime_error("Discovery socket configuration failed.");

                #else
                const int flags = ::fcntl(socket, F_GETFL, 0);

                if (flags < 0 || ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0)
                    throw std::runtime_error("Discovery socket configuration failed.");

                #endif
                const auto name = local_device_name();
                auto window = std::chrono::steady_clock::now();
                int replies = 0;

                while (!token.stop_requested()) {

                    fd_set readable;
                    FD_ZERO(&readable);
                    FD_SET(socket, &readable);
                    timeval timeout{0, 100000};

                    #ifdef _WIN32
                    const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
                    #else
                    const int ready = ::select(socket + 1, &readable, nullptr, nullptr, &timeout);

                    #endif
                    if (ready == 0)
                        continue;

                    if (ready < 0) {

                        #ifdef _WIN32
                        if (WSAGetLastError() == WSAEINTR)
                            continue;

                        #else
                        if (errno == EINTR)
                            continue;

                        #endif
                        throw std::runtime_error("Desktop discovery wait failed.");

                    }

                    std::array<char, 128> request{};
                    sockaddr_in sender{};
                    #ifdef _WIN32
                    int length = sizeof(sender);

                    #else
                    socklen_t length = sizeof(sender);
                    #endif
                    const auto size = ::recvfrom(
                        socket,

                        request.data(),
                        static_cast<int>(request.size()),
                        0,

                        reinterpret_cast<sockaddr*>(&sender),
                        &length
                    );

                    if (size != 16 || std::memcmp(request.data(), "CDDISC01", 8) != 0)
                        continue;

                    const auto now = std::chrono::steady_clock::now();

                    if (now - window >= std::chrono::seconds(1)) {

                        window = now;
                        replies = 0;

                    }

                    if (++replies > 20)
                        continue;

                    std::vector<char> response{'C', 'D', 'P', 'E', 'E', 'R', '0', '1'};
                    response.insert(response.end(), request.begin() + 8, request.begin() + 16);
                    response.push_back(static_cast<char>(port >> 8));
                    response.push_back(static_cast<char>(port));
                    response.push_back(static_cast<char>(protocol::version));

                    response.push_back(busy ? 1 : 0);
                    response.push_back(static_cast<char>(name.size()));
                    response.insert(response.end(), name.begin(), name.end());
                    ::sendto(
                        socket,

                        response.data(),
                        static_cast<int>(response.size()),
                        0,
                        reinterpret_cast<sockaddr*>(&sender),
                        length
                    );

                }

            }

        };

        static bool terminal_fin(const protocol::packet& pkt) {

            constexpr std::array<std::uint8_t, 7> marker{1, 0, 0xAE, 0xEF, 0, 0x11, 0xFF};

            return pkt.type == protocol::PacketType::Communication && pkt.has_flag(protocol::ProtocolFlag::ACK) &&
                   pkt.has_flag(protocol::ProtocolFlag::FIN) && pkt.payload.size() == marker.size() &&
                   std::equal(pkt.payload.begin(), pkt.payload.end(), marker.begin());

        }

    };

    application::application(application_config config, application_callbacks handlers)
        : configuration(config), callbacks(std::move(handlers)) {

        if (configuration.port < 4097 || configuration.handshake_timeout_ms <= 0)
            throw std::invalid_argument("Invalid listening port or handshake timeout.");

        decoder_preference = configuration.decoder;
        automatic_decoder = configuration.automatic_decoder;

    }

    void application::request_video(protocol::video_format format, std::uint32_t bitrate) {
        (void)protocol::make_video_control({1, format, 0, bitrate});
        std::lock_guard lock(control_mutex);
        requested_control = protocol::video_control{1, format, 0, bitrate};
    }

    void application::request_decoder(const std::string& id) {
        std::lock_guard lock(decoder_mutex);
        decoder_preference = id.empty() ? "auto" : id;
    }

    void application::flip_camera() {
        std::lock_guard lock(control_mutex);
        flip_requested = true;
    }

    void application::identify_decoder() {
        std::lock_guard lock(decoder_mutex);
        ++decoder_identification;
    }

    void application::set_automatic_decoder(bool enabled) {
        std::lock_guard lock(decoder_mutex);
        automatic_decoder = enabled;
    }

    void application::run_connection(const std::shared_ptr<tcp_socket>& phone) {

        constexpr std::size_t max_bytes = 120 * 1024 * 1024;
        constexpr std::size_t max_packets = 32;
        std::mutex mutex;
        std::condition_variable changed;
        using clock = std::chrono::steady_clock;

        std::deque<std::pair<protocol::packet, clock::time_point>> packets;
        std::atomic<bool> recovery_requested{false};

        std::size_t queued_bytes = 0;
        bool input_finished = false;
        bool abort_decode = false;
        std::exception_ptr decode_error;
        std::atomic<std::uint64_t> decoded_frames{0};

        phone->set_low_latency();
        video_receiver video;
        bool awaiting_video_keyframe = true;
        bool video_active = false;
        std::uint64_t last_video_id = 0;

        std::optional<std::uint64_t> last_video_pts;
        auto recovery_deadline = std::chrono::steady_clock::time_point::min();
        auto feedback_deadline = clock::now() + std::chrono::seconds(1);
        protocol::video_feedback feedback;

        std::jthread decoding([&] {

            try {

                std::uint64_t last_identification = 0;
                bool decoder_waiting_keyframe = true;
                double last_decode_ms = 0;
                std::map<std::int64_t, std::uint64_t> frame_ids;
                std::map<std::uint64_t, std::uint16_t> orientations;

                video_decoder decoder(
                    [&](const AVFrame& frame) {

                        const auto count = ++decoded_frames;
                        const auto pts = frame.pts != AV_NOPTS_VALUE ? frame.pts : frame.best_effort_timestamp;
                        const auto identity = frame_ids.find(pts);

                        if (identity != frame_ids.end()) {

                            auto orientation = orientations.upper_bound(identity->second);

                            if (orientation != orientations.begin()) {

                                const auto rotation = std::to_string(std::prev(orientation)->second);
                                if (av_dict_set(
                                        &const_cast<AVFrame&>(frame).metadata,
                                        "camdroid.rotation",
                                        rotation.c_str(),

                                        0
                                    ) < 0)
                                    throw std::bad_alloc();

                            }

                        }

                        if (callbacks.on_frame)
                            callbacks.on_frame(frame, count);

                    },

                    [&](const decoder_selection& selection) {

                        if (selection.message == "Selected best for you") {

                            std::lock_guard lock(decoder_mutex);
                            decoder_preference = selection.active;

                        }

                        if (callbacks.on_decoder_selection)
                            callbacks.on_decoder_selection(selection);

                    },

                    [&] {
                        return cancelled();
                    }
                );

                while (true) {

                    protocol::packet pkt;
                    clock::time_point arrived;

                    {

                        std::unique_lock lock(mutex);

                        changed.wait(lock, [&] {
                            return cancelled() || abort_decode || input_finished || !packets.empty();
                        });

                        if (cancelled() || abort_decode)
                            return;

                        if (packets.empty())
                            break;

                        arrived = packets.front().second;
                        pkt = std::move(packets.front().first);
                        queued_bytes -= pkt.payload.size() + pkt.extensions.size();
                        packets.pop_front();

                    }

                    changed.notify_all();

                    std::string preference;
                    bool automatic;
                    std::uint64_t identification;

                    {

                        std::lock_guard lock(decoder_mutex);
                        preference = decoder_preference;
                        automatic = automatic_decoder;
                        identification = decoder_identification;

                    }

                    const auto age = clock::now() - arrived;
                    const bool is_frame = pkt.payload_flags == 0 || pkt.payload_flags == 2;

                    if (is_frame &&
                        (age > std::chrono::milliseconds(75) || (decoder_waiting_keyframe && pkt.payload_flags == 0))) {

                        if (!decoder_waiting_keyframe)
                            decoder.discontinuity();

                        decoder_waiting_keyframe = true;
                        recovery_requested = true;

                        if (callbacks.on_video_timing)
                            callbacks.on_video_timing(std::chrono::duration<double, std::milli>(age).count(), last_decode_ms);

                        continue;

                    }

                    if (is_frame)
                        decoder_waiting_keyframe = false;

                    const auto decode_started = clock::now();

                    decoder.set_preference(preference);
                    decoder.set_automatic(automatic);

                    if (identification != last_identification) {

                        last_identification = identification;
                        decoder.identify_best();

                    }

                    switch (pkt.payload_flags) {

                        case 0x81: {

                            pkt.type = protocol::PacketType::Communication;
                            pkt.payload_flags = 0;
                            const auto update = protocol::parse_video_orientation(pkt);
                            orientations[update.first_frame] = update.rotation;
                            while (orientations.size() > 256)
                                orientations.erase(orientations.begin());

                            break;
                        }

                        case 0x80:
                            // Local queue marker, never accepted from the wire.
                            decoder.discontinuity();
                            decoder_waiting_keyframe = true;
                            break;

                        case static_cast<std::uint8_t>(protocol::VideoFlag::CONFIG): {
                            const auto config = protocol::video_payload::parse_config(pkt.payload);
                            // configure() drains the old stream using its old orientation.
                            decoder.configure(config);
                            frame_ids.clear();

                            orientations.clear();
                            decoder_waiting_keyframe = true;

                            if (callbacks.on_video_config)
                                callbacks.on_video_config(config);

                            if (callbacks.on_video_settings)
                                callbacks.on_video_settings(config.settings);

                            break;
                        }
                        case static_cast<std::uint8_t>(protocol::VideoFlag::SETTINGS): {
                            const auto settings = protocol::video_payload::parse_settings(pkt.payload);
                            decoder.update_settings(settings);

                            if (callbacks.on_video_settings)
                                callbacks.on_video_settings(settings);

                            break;
                        }
                        case 0:
                        case static_cast<std::uint8_t>(protocol::VideoFlag::KEYFRAME): {
                            const auto footage = protocol::video_payload::parse_frame(pkt.payload);

                            if (pkt.video_frame_id) {

                                frame_ids[static_cast<std::int64_t>(footage.pts_us)] = pkt.video_frame_id;
                                while (frame_ids.size() > 512)
                                    frame_ids.erase(frame_ids.begin());

                            }

                            decoder.decode(footage, pkt.payload_flags != 0);

                            break;
                        }
                        case static_cast<std::uint8_t>(protocol::VideoFlag::EOS):
                            decoder.drain();

                            if (callbacks.on_video_ended)
                                callbacks.on_video_ended(decoded_frames);

                            break;
                        default:
                            throw std::runtime_error("Unsupported Video payload flags.");

                    }

                    if (is_frame) {

                        last_decode_ms = std::chrono::duration<double, std::milli>(clock::now() - decode_started).count();

                        if (callbacks.on_video_timing)
                            callbacks.on_video_timing(std::chrono::duration<double, std::milli>(age).count(), last_decode_ms);

                    }

                }

                decoder.drain();

            } catch (...) {

                {

                    std::lock_guard lock(mutex);
                    decode_error = std::current_exception();

                }

                phone->interrupt();
                changed.notify_all();

            }

        });

        std::exception_ptr receive_error;

        try {

            protocol::session connection;
            connection.start();
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(configuration.handshake_timeout_ms);
            bool established = false;

            std::optional<protocol::video_control> pending_control;
            std::optional<std::uint32_t> pending_flip;
            auto flip_deadline = std::chrono::steady_clock::now();
            auto control_deadline = std::chrono::steady_clock::now();
            std::array<std::byte, 320 * 1024> buffer{};

            while (!cancelled()) {

                {

                    std::lock_guard lock(mutex);

                    if (decode_error)
                        std::rethrow_exception(decode_error);

                }

                if (!established && std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Protocol handshake timed out.");

                if (pending_control && std::chrono::steady_clock::now() >= control_deadline) {

                    if (callbacks.on_negotiation)
                        callbacks.on_negotiation(
                            "Phone did not complete the requested capture settings; retaining the current stream."
                        );

                    if (callbacks.on_video_request_result) {

                        auto timeout = *pending_control;
                        timeout.status = 3;
                        callbacks.on_video_request_result(timeout);

                    }

                    pending_control.reset();

                }

                if (pending_flip && clock::now() >= flip_deadline) {

                    pending_flip.reset();

                    if (callbacks.on_negotiation)
                        callbacks.on_negotiation("Camera flip timed out; check the phone.");

                }

                if (established && !pending_control && !pending_flip) {

                    bool flip;

                    {

                        std::lock_guard lock(control_mutex);
                        flip = std::exchange(flip_requested, false);

                    }

                    if (flip) {

                        if (++next_request == 0)
                            ++next_request;

                        pending_flip = next_request;
                        auto query = protocol::make_camera_flip(*pending_flip);
                        query.set_flag(protocol::ProtocolFlag::OPTS, connection.is_optimized());
                        const auto encoded = connection.send(query);
                        phone->send_all(encoded.data(), encoded.size());
                        flip_deadline = clock::now() + std::chrono::seconds(12);

                    }

                }

                if (established && !pending_control && !pending_flip) {

                    std::optional<protocol::video_control> control;

                    {

                        std::lock_guard lock(control_mutex);
                        control = std::exchange(requested_control, std::nullopt);

                    }

                    if (control) {

                        if (++next_request == 0)
                            ++next_request;

                        control->id = next_request;
                        pending_control = *control;
                        auto query = protocol::make_video_control(*pending_control);
                        query.set_flag(protocol::ProtocolFlag::OPTS, connection.is_optimized());
                        const auto encoded = connection.send(query);

                        phone->send_all(encoded.data(), encoded.size());
                        control_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);

                        if (callbacks.on_negotiation)
                            callbacks.on_negotiation(
                                "Requested phone capture: " + std::to_string(control->format.width) + " x " +
                                std::to_string(control->format.height) + " at " + std::to_string(control->format.fps) + " FPS"
                            );

                    }

                }

                if (established && recovery_requested && clock::now() >= recovery_deadline) {

                    recovery_requested = false;
                    awaiting_video_keyframe = true;
                    auto recovery = protocol::video_datagram::control(protocol::video_datagram::recover, 1);
                    recovery.set_flag(protocol::ProtocolFlag::OPTS, connection.is_optimized());
                    const auto encoded = connection.send(recovery);

                    phone->send_all(encoded.data(), encoded.size());
                    recovery_deadline = clock::now() + std::chrono::milliseconds(500);

                }

                if (established && video_active && clock::now() >= feedback_deadline) {

                    {

                        std::lock_guard lock(mutex);
                        const auto age =
                            packets.empty()
                                ? 0LL
                                : std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - packets.front().second)
                                      .count();

                        feedback.queue_ms = static_cast<std::uint16_t>(std::clamp<long long>(age, 0, 65535));

                    }

                    if (recovery_requested)
                        feedback.queue_ms = std::max<std::uint16_t>(feedback.queue_ms, 75);

                    auto report = protocol::make_video_feedback(feedback);
                    report.set_flag(protocol::ProtocolFlag::OPTS, connection.is_optimized());
                    const auto encoded = connection.send(report);
                    phone->send_all(encoded.data(), encoded.size());
                    feedback = {};
                    feedback_deadline = clock::now() + std::chrono::seconds(1);

                }

                if (!phone->wait_readable(10))
                    continue;

                const auto bytes = phone->recv(buffer.data(), buffer.size());

                if (bytes == 0) {

                    if (!established)
                        throw std::runtime_error("Peer disconnected during handshake.");

                    if (connection.has_buffered_data())
                        throw std::runtime_error("Peer disconnected inside a protocol packet.");

                    break;

                }

                if (callbacks.on_received)
                    callbacks.on_received(bytes);

                connection.feed(buffer.data(), bytes);

                bool progress;

                do {

                    progress = false;

                    while (auto pkt = connection.next_packet()) {

                        progress = true;
                        const auto packet_arrived = clock::now();

                        if (cancelled())
                            break;

                        if (static_cast<std::uint8_t>(pkt->type) > static_cast<std::uint8_t>(protocol::PacketType::Video))
                            throw std::runtime_error("Unknown protocol packet type.");

                        if (pkt->type == protocol::PacketType::Communication && !pkt->payload.empty() &&
                            pkt->payload[0] == 0x15) {

                            (void)protocol::parse_video_orientation(*pkt);
                            pkt->type = protocol::PacketType::Video;
                            pkt->payload_flags = 0x81;
                            std::lock_guard lock(mutex);
                            if (packets.size() >= max_packets)
                                throw std::runtime_error("Too many queued orientation updates.");

                            queued_bytes += pkt->payload.size();
                            packets.push_back({std::move(*pkt), packet_arrived});
                            changed.notify_all();
                            continue;

                        }

                        if (pkt->type == protocol::PacketType::Communication && !pkt->payload.empty() &&
                            pkt->payload[0] == 0x13) {

                            const auto capabilities = protocol::parse_camera_capabilities(*pkt);
                            if (callbacks.on_camera_capabilities)
                                callbacks.on_camera_capabilities(capabilities);

                            continue;

                        }

                        if (pkt->type == protocol::PacketType::Communication && !pkt->payload.empty() &&
                            pkt->payload[0] == 0x14) {

                            const auto selected = protocol::parse_capture_selection(*pkt);
                            if (callbacks.on_capture_selection)
                                callbacks.on_capture_selection(selected);

                            continue;

                        }

                        if (pkt->type == protocol::PacketType::Communication && !pkt->payload.empty() &&
                            pkt->payload[0] == 0x11) {

                            const auto id = protocol::parse_camera_flip(*pkt);

                            if (!pkt->has_flag(protocol::ProtocolFlag::ACK))
                                throw std::runtime_error("Unexpected camera flip query.");

                            if (pending_flip && id == *pending_flip) {

                                if (callbacks.on_negotiation)
                                    callbacks.on_negotiation(
                                        pkt->payload.back() == 0 ? "Camera flipped." : "Phone could not flip the camera."
                                    );

                                pending_flip.reset();

                            }

                            continue;

                        }

                        if (pkt->type == protocol::PacketType::Communication && !pkt->payload.empty() &&
                            pkt->payload[0] == 0x10) {

                            const auto reply = protocol::parse_video_control(*pkt);

                            if (pending_control && pkt->has_flag(protocol::ProtocolFlag::ACK) &&
                                reply.id == pending_control->id && reply.format == pending_control->format &&
                                reply.bitrate == pending_control->bitrate) {

                                if (callbacks.on_negotiation)
                                    callbacks.on_negotiation(
                                        reply.status == 0
                                            ? "Phone applied the requested capture settings."
                                            : "Phone declined the capture settings; retaining the current stream."
                                    );

                                if (callbacks.on_video_request_result)
                                    callbacks.on_video_request_result(reply);

                                pending_control.reset();

                            }

                            continue;

                        }

                        namespace wire = protocol::video_datagram;

                        if (pkt->type == protocol::PacketType::Communication && !pkt->payload.empty() &&
                            pkt->payload[0] >= wire::setup && pkt->payload[0] <= wire::recover) {

                            wire::validate(*pkt);

                            if (!established || !video_active || pkt->payload[0] != wire::frame)
                                throw std::runtime_error("Unexpected UDP video control packet.");

                            const auto data = std::span<const std::uint8_t>(pkt->payload);
                            const auto id = wire::get(data.subspan(1, 8));
                            const auto size = static_cast<std::size_t>(wire::get(data.subspan(9, 4)));
                            const bool keyframe = data[13] != 0;

                            if (id <= last_video_id)
                                throw std::runtime_error("UDP frame descriptors are out of order.");

                            ++feedback.expected;
                            last_video_id = id;
                            const auto flags = data[13];

                            auto footage = video.take(
                                id,
                                size,
                                !awaiting_video_keyframe || keyframe,
                                packet_arrived + std::chrono::milliseconds(keyframe ? 75 : 20),

                                keyframe,
                                [&] {
                                    return cancelled();
                                }
                            );

                            if (footage.empty() || (awaiting_video_keyframe && !keyframe)) {

                                if (!awaiting_video_keyframe) {

                                    protocol::packet reset;
                                    reset.type = protocol::PacketType::Video;
                                    reset.payload_flags = 0x80;
                                    std::lock_guard lock(mutex);
                                    packets.push_back({std::move(reset), clock::now()});
                                    changed.notify_all();

                                }

                                awaiting_video_keyframe = true;
                                const auto now = std::chrono::steady_clock::now();

                                if (now >= recovery_deadline) {

                                    auto recovery = wire::control(wire::recover, 1);
                                    recovery.set_flag(protocol::ProtocolFlag::OPTS, connection.is_optimized());
                                    const auto encoded = connection.send(recovery);
                                    phone->send_all(encoded.data(), encoded.size());
                                    recovery_deadline = now + std::chrono::milliseconds(500);

                                }

                                continue;

                            }

                            ++feedback.usable;
                            pkt->type = protocol::PacketType::Video;
                            pkt->payload_flags = flags;
                            pkt->video_frame_id = id;
                            pkt->payload = std::move(footage);

                            protocol::video_payload::validate(*pkt);
                            const auto pts = wire::get(std::span<const std::uint8_t>(pkt->payload).first(8));

                            if (last_video_pts && pts < *last_video_pts)
                                throw std::runtime_error("UDP video timestamps moved backwards.");

                            last_video_pts = pts;
                            awaiting_video_keyframe = false;

                            if (callbacks.on_received)
                                callbacks.on_received(pkt->payload.size());

                        }

                        if (pkt->type != protocol::PacketType::Video)
                            continue;

                        if (pkt->payload_flags == static_cast<std::uint8_t>(protocol::VideoFlag::CONFIG)) {

                            awaiting_video_keyframe = true;
                            video_active = true;
                            feedback = {};
                            feedback_deadline = clock::now() + std::chrono::seconds(1);
                            last_video_pts.reset();

                        }

                        if (pkt->payload_flags == static_cast<std::uint8_t>(protocol::VideoFlag::EOS)) {

                            awaiting_video_keyframe = true;
                            video_active = false;

                        }

                        if (callbacks.on_encoded_frame &&
                            (pkt->payload_flags == 0 ||
                             pkt->payload_flags == static_cast<std::uint8_t>(protocol::VideoFlag::KEYFRAME)))
                            callbacks.on_encoded_frame(pkt->payload.size() - 8);

                        const auto size = pkt->payload.size() + pkt->extensions.size();

                        if (size > max_bytes)
                            throw std::runtime_error("Video packet exceeds decoder queue limit.");

                        std::unique_lock lock(mutex);

                        if ((pkt->payload_flags == 0 || pkt->payload_flags == 2) &&
                            (queued_bytes > max_bytes - size || packets.size() >= max_packets ||
                             std::any_of(packets.begin(), packets.end(), [&](const auto& entry) {
                                 return (entry.first.payload_flags == 0 || entry.first.payload_flags == 2) &&
                                        clock::now() - entry.second > std::chrono::milliseconds(75);
                             }))) {

                            // Preserve stream boundaries/settings, but don't retain stale encoded references.
                            for (auto it = packets.begin(); it != packets.end();) {

                                if (it->first.payload_flags == 0 || it->first.payload_flags == 2 ||
                                    it->first.payload_flags == 0x80) {

                                    queued_bytes -= it->first.payload.size() + it->first.extensions.size();
                                    it = packets.erase(it);

                                } else {

                                    ++it;

                                }

                            }

                            protocol::packet reset;
                            reset.type = protocol::PacketType::Video;
                            reset.payload_flags = 0x80;
                            packets.push_back({std::move(reset), clock::now()});
                            awaiting_video_keyframe = true;

                            recovery_requested = true;
                            changed.notify_all();

                            continue;

                        }

                        while (!cancelled() && !decode_error &&
                               (queued_bytes > max_bytes - size || packets.size() >= max_packets))
                            changed.wait_for(lock, std::chrono::milliseconds(100));

                        if (decode_error)
                            std::rethrow_exception(decode_error);

                        if (cancelled())
                            break;

                        queued_bytes += size;
                        packets.push_back({std::move(*pkt), packet_arrived});
                        lock.unlock();
                        changed.notify_all();

                    }

                    while (!cancelled()) {

                        auto response = connection.next_response();

                        if (!response)
                            break;

                        progress = true;
                        const bool terminal = connection.is_terminal_fin_pending() && utils::terminal_fin(*response);
                        const auto encoded = connection.send(*response);
                        phone->send_all(encoded.data(), encoded.size());

                        if (terminal)
                            connection.on_terminal_fin_sent();

                    }

                    if (connection.get_state() == protocol::SessionState::Closing)
                        throw std::runtime_error("Protocol exchange was rejected.");

                    if (!established && connection.get_state() == protocol::SessionState::Established) {

                        established = true;
                        auto offer = video.offer();
                        offer.set_flag(protocol::ProtocolFlag::OPTS, connection.is_optimized());
                        const auto encoded = connection.send(offer);
                        phone->send_all(encoded.data(), encoded.size());

                        if (callbacks.on_established)
                            callbacks.on_established();

                    }

                } while (progress && !cancelled());

            }

            connection.close();

        } catch (...) {

            receive_error = std::current_exception();

        }

        {

            std::lock_guard lock(mutex);
            input_finished = true;
            abort_decode = receive_error != nullptr || cancelled();

            if (abort_decode)
                packets.clear();

        }

        changed.notify_all();
        decoding.join();

        if (!stop_requested && callbacks.on_disconnected)
            callbacks.on_disconnected(decoded_frames);

        {

            std::lock_guard lock(control_mutex);
            requested_control.reset();
            flip_requested = false;

        }

        if (!cancelled()) {

            if (decode_error)
                std::rethrow_exception(decode_error);

            if (receive_error)
                std::rethrow_exception(receive_error);

        }

    }

    void application::disconnect_peer() noexcept {

        peer_cancelled = true;
        std::lock_guard lock(socket_mutex);

        if (active_socket)
            active_socket->interrupt();

    }

    void application::request_stop() noexcept {
        stop_requested = true;
        disconnect_peer();
    }

    void application::run() {

        if (started.exchange(true))
            throw std::logic_error("This application instance has already run.");

        if (stop_requested)
            return;

        tcp_socket server(true, 5);
        const auto port = server.bind(configuration.port);
        server.listen();
        server.set_nonblocking(true);
        std::atomic<bool> busy{false};

        std::jthread discovery([&](std::stop_token token) {

            try {

                utils::receiver_discovery service;
                service.run(token, port, busy);

            } catch (const std::exception& error) {

                if (!stop_requested && callbacks.on_error)
                    callbacks.on_error(error.what());

            }

        });

        while (!stop_requested) {

            if (callbacks.on_listening)
                callbacks.on_listening(port);

            std::optional<tcp_socket> accepted;

            while (!stop_requested && !accepted) {

                if (server.wait_readable(100))
                    accepted = server.try_accept();

            }

            if (stop_requested)
                break;

            peer_cancelled = false;
            busy = true;
            auto phone = std::make_shared<tcp_socket>(std::move(*accepted));

            {

                std::lock_guard lock(socket_mutex);
                active_socket = phone;

            }

            if (stop_requested) {

                phone->interrupt();
                break;

            }

            std::exception_ptr failure;

            try {

                phone->set_send_timeout(1000);

                if (callbacks.on_connected)
                    callbacks.on_connected();

                run_connection(phone);

            } catch (...) {

                failure = std::current_exception();

            }

            phone->interrupt();

            {

                std::lock_guard lock(socket_mutex);
                active_socket.reset();

            }

            phone.reset();
            busy = false;

            if (failure && !cancelled()) {

                try {

                    std::rethrow_exception(failure);

                } catch (const std::exception& error) {

                    if (callbacks.on_error)
                        callbacks.on_error(error.what());

                }

            }

            if (!configuration.keep_listening)
                break;

        }

    }

}