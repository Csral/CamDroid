#include "native_transport.h"
#include "packet_builder.h"
#include "video_payload.h"
#include "video_control.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>

#include <stdexcept>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>

#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/multinetwork.h>
#endif

namespace camdroid::android {

    namespace utils {

        constexpr std::size_t max_payload = 80 * 1024 * 1024;
        constexpr std::size_t max_queued_bytes = 120 * 1024 * 1024;
        constexpr std::size_t max_queued_packets = 32;

        static std::uint64_t timestamp_ms() {

            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count()
            );

        }

        static bool terminal_fin(const protocol::packet& pkt) {

            constexpr std::array<std::uint8_t, 7> marker{1, 0, 0xAE, 0xEF, 0, 0x11, 0xFF};

            return pkt.type == protocol::PacketType::Communication && pkt.has_flag(protocol::ProtocolFlag::ACK) &&
                   pkt.has_flag(protocol::ProtocolFlag::FIN) && pkt.payload.size() == marker.size() &&
                   std::equal(pkt.payload.begin(), pkt.payload.end(), marker.begin());

        }

        static bool video_frame(const protocol::packet& pkt) {
            return pkt.type == protocol::PacketType::Video &&
                   (pkt.payload_flags == 0 || pkt.payload_flags == static_cast<std::uint8_t>(protocol::VideoFlag::KEYFRAME));
        }

        static void send_all(int fd, std::span<const std::uint8_t> data) {

            std::size_t offset = 0;

            while (offset < data.size()) {

                const ssize_t count = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);

                if (count < 0 && errno == EINTR)
                    continue;

                // SO_SNDTIMEO wakes a stalled writer periodically. A slow reader
                // isn't a broken connection; preserve this in-flight packet's tail.
                // shutdown() on explicit close still interrupts the blocking write.
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    continue;

                if (count <= 0)
                    throw std::runtime_error("Desktop socket write failed.");

                offset += static_cast<std::size_t>(count);

            }

        }

    };

    native_transport::~native_transport() {

        close();

        if (video_fd >= 0)
            ::close(video_fd);

        const int fd = socket_fd.exchange(-1);

        if (fd >= 0)
            ::close(fd);

    }

    void native_transport::stop(const std::string& reason) {

        {

            std::lock_guard lock(queue_mutex);

            if (!stopped.exchange(true))
                failure = reason;

            established = false;
            outgoing_packets.clear();
            incoming_packets.clear();
            outgoing_bytes = 0;
            incoming_bytes = 0;

        }

        const int fd = socket_fd.load();

        if (fd >= 0)
            ::shutdown(fd, SHUT_RDWR);

        changed.notify_all();

    }

    void native_transport::close() {

        stop("Connection closed.");
        // Serializes worker startup/join. No worker calls close() or joins itself.
        std::lock_guard lock(thread_mutex);

        if (reader.joinable())
            reader.join();

        if (writer.joinable())
            writer.join();

    }

    bool native_transport::is_connected() const noexcept {
        return established.load() && !stopped.load();
    }

    bool native_transport::needs_video_keyframe() const noexcept {
        return keyframe_needed.load();
    }

    void native_transport::interrupt() {
        stop("Connection closed.");
    }

    std::string native_transport::error() {
        std::lock_guard lock(queue_mutex);

        return failure;
    }

    void native_transport::connect(const std::string& host, int port, bool optimized, std::uint64_t network) {

        if (port < 1 || port > 65535)
            throw std::invalid_argument("Invalid desktop port.");

        {

            std::lock_guard lock(queue_mutex);

            if (started || stopped)
                throw std::runtime_error("Use a new connection after disconnecting.");

            started = true;

        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));

        if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
            throw std::invalid_argument("Enter a numeric IPv4 desktop address.");

        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

        if (fd < 0)
            throw std::runtime_error("Could not create desktop socket.");

        {

            std::lock_guard lock(queue_mutex);

            if (stopped) {

                ::close(fd);
                throw std::runtime_error("Connection cancelled.");

            }

            socket_fd = fd;

        }

        try {

            #ifdef __ANDROID__
            if (network != 0 && android_setsocknetwork(static_cast<net_handle_t>(network), fd) != 0)
                throw std::runtime_error("Could not select the discovered desktop network.");

            #else
            if (network != 0)
                throw std::invalid_argument("Network binding requires Android.");

            #endif
            video_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);

            if (video_fd < 0)
                throw std::runtime_error("Could not create UDP video socket.");

            #ifdef __ANDROID__
            if (network != 0 && android_setsocknetwork(static_cast<net_handle_t>(network), video_fd) != 0)
                throw std::runtime_error("Could not select the UDP video network.");

            #endif

            if (::fcntl(video_fd, F_SETFL, O_NONBLOCK) < 0)
                throw std::runtime_error("Could not configure UDP video socket.");

            video_address = address;

            const int flags = ::fcntl(fd, F_GETFL, 0);

            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
                throw std::runtime_error("Could not configure desktop socket.");

            if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {

                if (errno != EINPROGRESS)
                    throw std::runtime_error("Desktop connection failed.");

                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

                while (true) {

                    if (stopped)
                        throw std::runtime_error("Connection cancelled.");

                    pollfd event{fd, POLLOUT, 0};
                    const int ready = ::poll(&event, 1, 100);

                    if (ready < 0 && errno == EINTR)
                        continue;

                    if (ready < 0)
                        throw std::runtime_error("Desktop connection poll failed.");

                    if (ready > 0) {

                        int result = 0;
                        socklen_t length = sizeof(result);

                        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &length) < 0 || result != 0)
                            throw std::runtime_error("Desktop connection failed.");

                        break;

                    }

                    if (std::chrono::steady_clock::now() >= deadline)
                        throw std::runtime_error("Desktop connection timed out.");

                }

            }

            if (stopped || ::fcntl(fd, F_SETFL, flags) < 0)
                throw std::runtime_error("Connection cancelled or socket configuration failed.");

            const int low_latency = 1;

            if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &low_latency, sizeof(low_latency)) < 0)
                throw std::runtime_error("Could not configure low latency TCP controls.");

            timeval timeout{5, 0};

            if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
                throw std::runtime_error("Could not configure write timeout.");

            {

                std::lock_guard lock(session_mutex);
                connection.start();
                protocol::packet_builder builder;

                const std::array<std::uint8_t, 4> payload{
                    1,
                    protocol::min_supported_version,
                    protocol::max_supported_version,

                    static_cast<std::uint8_t>(optimized ? 0x80 : 0)
                };
                enqueue({builder.type(protocol::PacketType::Communication)
                             .flag(protocol::ProtocolFlag::QRY)
                             .payload(payload)
                             .build()});

            }

            {

                std::lock_guard lock(thread_mutex);

                if (stopped)
                    throw std::runtime_error("Connection cancelled.");

                writer = std::thread(&native_transport::write_loop, this);
                reader = std::thread(&native_transport::read_loop, this);

            }

            std::unique_lock lock(queue_mutex);

            if (!changed.wait_for(lock, std::chrono::seconds(10), [&] {
                    return (established && video_ready) || stopped;
                }))

                throw std::runtime_error("Protocol handshake or UDP video setup timed out.");

            if (stopped)
                throw std::runtime_error(failure);

        } catch (...) {

            close();
            throw;

        }

    }

    bool native_transport::enqueue(outgoing item) {

        const std::size_t size = item.pkt.payload.size() + item.pkt.extensions.size() + 28;
        std::lock_guard lock(queue_mutex);

        if (stopped)
            throw std::runtime_error(failure);

        const bool frame = utils::video_frame(item.pkt);
        const bool keyframe = frame && item.pkt.payload_flags == static_cast<std::uint8_t>(protocol::VideoFlag::KEYFRAME);

        if (recovering_video && frame && !keyframe)
            return false;

        const auto stale = std::any_of(outgoing_packets.begin(), outgoing_packets.end(), [](const auto& queued) {
            return utils::video_frame(queued.pkt) &&
                   std::chrono::steady_clock::now() - queued.queued > std::chrono::milliseconds(75);
        });

        if (stale || size > utils::max_queued_bytes - outgoing_bytes ||
            outgoing_packets.size() >= utils::max_queued_packets) {

            // The writer's in-flight packet and kernel-buffered bytes cannot be
            // retracted. Remove only complete queued footage, never stream/control
            // boundaries. No dependent frames may follow the resulting gap.
            for (auto it = outgoing_packets.begin(); it != outgoing_packets.end();) {

                if (utils::video_frame(it->pkt)) {

                    outgoing_bytes -= it->pkt.payload.size() + it->pkt.extensions.size() + 28;
                    it = outgoing_packets.erase(it);
                    recovering_video = true;
                    keyframe_needed = true;

                } else {

                    ++it;

                }

            }

            if (frame) {

                recovering_video = true;
                keyframe_needed = true;

                if (!keyframe)
                    return false;

            }

            if (size > utils::max_queued_bytes - outgoing_bytes || outgoing_packets.size() >= utils::max_queued_packets) {

                if (frame)
                    return false;

                throw std::runtime_error("Outgoing control/configuration queue is full.");

            }

        }

        outgoing_bytes += size;
        outgoing_packets.push_back(std::move(item));

        if (keyframe) {

            recovering_video = false;
            keyframe_needed = false;

        }

        changed.notify_all();

        return true;

    }

    void native_transport::process_buffered() {

        while (auto pkt = connection.next_packet()) {

            if (static_cast<std::uint8_t>(pkt->type) > static_cast<std::uint8_t>(protocol::PacketType::Video))
                throw std::runtime_error("Peer sent an unsupported packet type.");

            if (connection.get_state() == protocol::SessionState::Established &&
                pkt->type == protocol::PacketType::Communication && !pkt->payload.empty()) {

                namespace wire = protocol::video_datagram;

                if (pkt->payload[0] == wire::setup) {

                    wire::validate(*pkt);
                    std::lock_guard lock(queue_mutex);

                    if (video_ready)
                        throw std::runtime_error("Duplicate UDP video setup.");

                    video_address.sin_port = htons(
                        static_cast<std::uint16_t>(wire::get(std::span<const std::uint8_t>(pkt->payload).subspan(1, 2)))
                    );
                    std::copy(pkt->payload.begin() + 3, pkt->payload.end(), video_token.begin());
                    video_ready = true;
                    changed.notify_all();

                    continue;

                }

                if (pkt->payload[0] == wire::recover) {

                    wire::validate(*pkt);
                    std::lock_guard lock(queue_mutex);
                    recovering_video = true;
                    keyframe_needed = true;

                    for (auto it = outgoing_packets.begin(); it != outgoing_packets.end();) {

                        if (utils::video_frame(it->pkt)) {

                            outgoing_bytes -= it->pkt.payload.size() + it->pkt.extensions.size() + 28;
                            it = outgoing_packets.erase(it);

                        } else {

                            ++it;

                        }

                    }

                    continue;

                }

                if (pkt->payload[0] == wire::frame)
                    throw std::runtime_error("Unexpected incoming UDP frame descriptor.");

            }

            // Handshake packets are internal. Other accepted packets are delivered
            // in wire order to the Kotlin control worker, without native callbacks.
            if (established && connection.get_state() != protocol::SessionState::Closing) {

                std::lock_guard lock(queue_mutex);

                if (pkt->payload.size() > 20 * 1024 * 1024 - incoming_bytes || incoming_packets.size() >= 320)
                    throw std::runtime_error("Incoming control queue is full.");

                incoming_bytes += pkt->payload.size();
                incoming_packets.push_back(std::move(*pkt));
                changed.notify_all();

            }

            if (connection.get_state() == protocol::SessionState::Established) {

                std::lock_guard lock(queue_mutex);

                if (!stopped)
                    established = true;

                changed.notify_all();

            }

        }

        bool response_queued = false;

        while (auto response = connection.next_response()) {

            const bool terminal = connection.is_terminal_fin_pending() && utils::terminal_fin(*response);
            const bool closing = connection.get_state() == protocol::SessionState::Closing;
            enqueue({std::move(*response), terminal, closing});
            response_queued = true;

        }

        if (connection.get_state() == protocol::SessionState::Closing && !response_queued)
            throw std::runtime_error("Peer rejected the protocol exchange.");

        if (connection.get_state() == protocol::SessionState::Established) {

            std::lock_guard lock(queue_mutex);

            if (!stopped)
                established = true;

            changed.notify_all();

        }

    }

    void native_transport::read_loop() noexcept {

        try {

            std::array<std::uint8_t, 320 * 1024> buffer{};

            while (!stopped) {

                const ssize_t count = ::recv(socket_fd, buffer.data(), buffer.size(), 0);

                if (count < 0 && errno == EINTR)
                    continue;

                if (count <= 0)
                    throw std::runtime_error("Desktop disconnected or socket read failed.");

                std::lock_guard lock(session_mutex);
                connection.feed(buffer.data(), static_cast<std::size_t>(count));
                process_buffered();

            }

        } catch (const std::exception& error) {

            stop(error.what());

        } catch (...) {

            stop("Unexpected native socket reader failure.");

        }

    }

    void native_transport::write_loop() noexcept {

        try {

            std::optional<std::uint16_t> sent_rotation;

            while (true) {

                outgoing item;

                {

                    std::unique_lock lock(queue_mutex);

                    changed.wait(lock, [&] {
                        return stopped || !outgoing_packets.empty();
                    });

                    if (stopped)
                        return;

                    item = std::move(outgoing_packets.front());
                    outgoing_packets.pop_front();
                    outgoing_bytes -= item.pkt.payload.size() + item.pkt.extensions.size() + 28;

                }

                if (utils::video_frame(item.pkt) &&
                    std::chrono::steady_clock::now() - item.queued > std::chrono::milliseconds(75)) {

                    std::lock_guard lock(queue_mutex);
                    recovering_video = true;
                    keyframe_needed = true;

                    for (auto it = outgoing_packets.begin(); it != outgoing_packets.end();) {

                        if (utils::video_frame(it->pkt)) {

                            outgoing_bytes -= it->pkt.payload.size() + it->pkt.extensions.size() + 28;
                            it = outgoing_packets.erase(it);

                        } else {

                            ++it;

                        }

                    }

                    continue;

                }

                namespace wire = protocol::video_datagram;
                std::vector<std::uint8_t> footage;
                std::uint64_t id = 0;

                if (utils::video_frame(item.pkt)) {

                    std::unique_lock lock(queue_mutex);

                    changed.wait_for(lock, std::chrono::seconds(2), [&] {
                        return stopped || video_ready;
                    });

                    if (stopped)
                        return;

                    if (!video_ready)
                        throw std::runtime_error("Desktop did not enable UDP video transport.");

                    if (item.pkt.payload.size() > wire::max_frame)
                        throw std::runtime_error("Encoded video frame exceeds the UDP transport limit.");

                    if (++frame_id == 0)
                        throw std::runtime_error("UDP frame counter exhausted.");

                    id = frame_id;
                    const auto flags = item.pkt.payload_flags;
                    footage = std::move(item.pkt.payload);
                    item.pkt = wire::control(wire::frame, 14);
                    wire::put(std::span(item.pkt.payload).subspan(1, 8), id);

                    wire::put(std::span(item.pkt.payload).subspan(9, 4), footage.size());
                    item.pkt.payload[13] = flags;

                }

                if (item.pkt.type == protocol::PacketType::Video &&
                    item.pkt.payload_flags == static_cast<std::uint8_t>(protocol::VideoFlag::CONFIG))
                    sent_rotation = protocol::video_payload::parse_config(item.pkt.payload).rotation;

                if (id && item.rotation && item.rotation != sent_rotation) {

                    auto update = protocol::make_video_orientation({id, *item.rotation});
                    std::vector<std::uint8_t> orientation_bytes;

                    {

                        std::lock_guard lock(session_mutex);
                        update.set_flag(protocol::ProtocolFlag::OPTS, connection.is_optimized());
                        update.timestamp_ms = utils::timestamp_ms();
                        orientation_bytes = connection.send(update);

                    }

                    utils::send_all(socket_fd, orientation_bytes);
                    sent_rotation = item.rotation;

                }

                std::vector<std::uint8_t> bytes;

                {

                    std::lock_guard lock(session_mutex);

                    if (established) {

                        item.pkt.set_flag(protocol::ProtocolFlag::OPTS, connection.is_optimized());
                        item.pkt.version = protocol::version;

                    }

                    item.pkt.timestamp_ms = utils::timestamp_ms();
                    bytes = connection.send(item.pkt);

                }

                // Serialization owns its wire bytes now; release the queued payload
                // before a potentially slow write rather than retaining two frames.
                std::vector<std::uint8_t>{}.swap(item.pkt.payload);
                // No session/queue lock is held during a blocking socket write.
                utils::send_all(socket_fd, bytes);

                if (!footage.empty()) {

                    std::array<std::uint8_t, 1200> datagram{};
                    std::copy_n("CDU1", 4, datagram.begin());
                    std::copy(video_token.begin(), video_token.end(), datagram.begin() + 4);
                    wire::put(std::span(datagram).subspan(20, 8), id);
                    wire::put(std::span(datagram).subspan(32, 4), footage.size());

                    for (std::size_t offset = 0; offset < footage.size() && !stopped; offset += wire::fragment_size) {

                        const auto length = std::min(wire::fragment_size, footage.size() - offset);
                        wire::put(std::span(datagram).subspan(28, 4), offset);
                        std::copy_n(footage.begin() + offset, length, datagram.begin() + wire::header_size);
                        ssize_t count;

                        do {

                            count = ::sendto(
                                video_fd,
                                datagram.data(),
                                wire::header_size + length,
                                MSG_NOSIGNAL,

                                reinterpret_cast<const sockaddr*>(&video_address),
                                sizeof(video_address)
                            );

                        } while (count < 0 && errno == EINTR && !stopped);

                        if (count != static_cast<ssize_t>(wire::header_size + length) ||
                            std::chrono::steady_clock::now() - item.queued > std::chrono::milliseconds(75)) {

                            // Never stall TCP controls behind a saturated UDP socket.
                            std::lock_guard lock(queue_mutex);
                            recovering_video = true;
                            keyframe_needed = true;

                            break;

                        }

                    }

                }

                if (item.terminal_fin) {

                    std::lock_guard lock(session_mutex);
                    connection.on_terminal_fin_sent();
                    process_buffered();

                }

                if (item.close_after) {

                    stop("Peer rejected the protocol exchange.");

                    return;

                }

            }

        } catch (const std::exception& error) {

            stop(error.what());

        } catch (...) {

            stop("Unexpected native socket writer failure.");

        }

    }

    void native_transport::send_packet(protocol::packet pkt) {

        if (!is_connected())
            throw std::runtime_error("Desktop session is not established.");

        if (pkt.payload.size() > utils::max_payload)
            throw std::length_error("Packet exceeds Android payload limit.");

        if (pkt.type == protocol::PacketType::Video)
            throw std::invalid_argument("Use the ordered video submission methods.");

        if (static_cast<std::uint8_t>(pkt.type) > static_cast<std::uint8_t>(protocol::PacketType::Video))
            throw std::invalid_argument("Unknown packet type.");

        enqueue({std::move(pkt)});

    }

    void native_transport::video_rotation(int rotation) {
        if (rotation < 0 || rotation > 270 || rotation % 90)
            throw std::invalid_argument("Invalid video rotation.");

        requested_rotation.store(rotation);
    }

    void native_transport::video_config(protocol::packet pkt) {

        std::lock_guard lock(video_mutex);

        if (!is_connected())
            throw std::runtime_error("Desktop session is not established.");

        if (pkt.payload.size() > utils::max_payload)
            throw std::length_error("Video configuration exceeds Android payload limit.");

        protocol::video_payload::validate(pkt);
        const int rotation = requested_rotation.load();

        if (rotation >= 0) {

            auto config = protocol::video_payload::parse_config(pkt.payload);
            config.rotation = static_cast<std::uint16_t>(rotation);
            pkt = protocol::video_payload::make_config(config);

        }

        pkt.set_flag(protocol::ProtocolFlag::NACK);
        enqueue({std::move(pkt)});
        pending_frame.clear();
        video_configured = true;

        awaiting_keyframe = true;
        first_pts.reset();

    }

    bool
    native_transport::video_fragment(std::span<const std::uint8_t> data, std::uint64_t pts, bool keyframe, bool partial) {

        std::lock_guard lock(video_mutex);

        if (!is_connected())
            throw std::runtime_error("Desktop session is not established.");

        if (!video_configured)
            throw std::runtime_error("Send video configuration before frames.");

        if (data.size() > utils::max_payload - 8 || pending_frame.size() > utils::max_payload - data.size())
            throw std::length_error("Encoded frame exceeds Android payload limit.");

        if (pending_frame.empty()) {

            if (data.empty())
                return false;

            pending_frame.resize(8);
            pending_pts = pts;
            pending_keyframe = false;

        }

        pending_keyframe = pending_keyframe || keyframe;
        pending_frame.insert(pending_frame.end(), data.begin(), data.end());

        if (partial)
            return false;

        if (awaiting_keyframe && !pending_keyframe) {

            pending_frame.clear();

            return false;

        }

        const auto origin = first_pts.value_or(pending_pts);

        if (pending_pts < origin)
            throw std::invalid_argument("Frame PTS precedes stream origin.");

        const auto relative = pending_pts - origin;

        for (int i = 0; i < 8; i++)
            pending_frame[i] = static_cast<std::uint8_t>(relative >> ((7 - i) * 8));
        protocol::packet pkt;
        pkt.type = protocol::PacketType::Video;
        pkt.payload_flags = pending_keyframe ? static_cast<std::uint8_t>(protocol::VideoFlag::KEYFRAME) : 0;

        pkt.set_flag(protocol::ProtocolFlag::NACK);
        pkt.payload = std::move(pending_frame);
        pending_frame.clear();
        outgoing item{std::move(pkt)};
        const int rotation = requested_rotation.load();

        if (rotation >= 0)
            item.rotation = static_cast<std::uint16_t>(rotation);

        const bool accepted = enqueue(std::move(item));

        if (accepted) {

            if (!first_pts)
                first_pts = pending_pts;

            awaiting_keyframe = false;

        }

        return accepted;

    }

    void native_transport::video_settings(int fps, int bitrate, bool user_configured) {

        std::lock_guard lock(video_mutex);

        if (!video_configured)
            return;

        if (fps < 1 || fps > 255 || bitrate < 1)
            throw std::invalid_argument("Invalid video settings.");

        auto pkt = protocol::video_payload::make_settings(
            {static_cast<std::uint16_t>(fps), static_cast<std::uint32_t>(bitrate), user_configured}
        );
        pkt.set_flag(protocol::ProtocolFlag::NACK);
        enqueue({std::move(pkt)});

    }

    void native_transport::video_eos() {

        std::lock_guard lock(video_mutex);

        if (!is_connected())
            throw std::runtime_error("Desktop session is not established.");

        if (!video_configured)
            return;

        // A stop may interrupt a partial access unit; discard that unfinished unit.
        pending_frame.clear();
        auto pkt = protocol::video_payload::make_eos();
        pkt.set_flag(protocol::ProtocolFlag::NACK);
        enqueue({std::move(pkt)});

        video_configured = false;
        first_pts.reset();

    }

    std::optional<protocol::packet> native_transport::wait_packet(int timeout_ms) {

        std::unique_lock lock(queue_mutex);

        changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return stopped || !incoming_packets.empty();
        });

        if (stopped)
            throw std::runtime_error(failure);

        if (incoming_packets.empty())
            return std::nullopt;

        auto pkt = std::move(incoming_packets.front());
        incoming_packets.pop_front();
        incoming_bytes -= pkt.payload.size();

        return pkt;

    }

}
