#include "native_transport.h"
#include "video_payload.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>

#include <iostream>
#include <stdexcept>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <sys/time.h>
#include <unistd.h>

using namespace camdroid;

namespace utils {

    static void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    struct test_server {

        int listener = -1;
        int port = 0;
        std::thread worker;
        std::exception_ptr failure;

        template <typename Function> explicit test_server(Function function) {

            listener = ::socket(AF_INET, SOCK_STREAM, 0);
            require(listener >= 0, "Test listener creation failed.");
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            require(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Test bind failed.");
            socklen_t size = sizeof(address);
            require(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0, "Test port lookup failed.");
            port = ntohs(address.sin_port);
            require(::listen(listener, 1) == 0, "Test listen failed.");

            worker = std::thread([this, function] {

                int peer = -1;

                try {

                    peer = ::accept(listener, nullptr, nullptr);
                    require(peer >= 0, "Test accept failed.");
                    timeval timeout{5, 0};
                    ::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                    function(peer);

                } catch (...) {

                    failure = std::current_exception();

                }

                if (peer >= 0)
                    ::close(peer);

            });

        }

        void finish() {
            worker.join();

            if (failure)
                std::rethrow_exception(failure);
        }

        ~test_server() {

            if (worker.joinable())
                worker.join();

            if (listener >= 0)
                ::close(listener);

        }

    };

    static void write_all(int fd, const std::vector<std::uint8_t>& bytes) {

        std::size_t offset = 0;

        while (offset < bytes.size()) {

            // Force fragmented handshakes/responses at the application layer.
            const ssize_t size =
                ::send(fd, bytes.data() + offset, std::min<std::size_t>(7, bytes.size() - offset), MSG_NOSIGNAL);
            require(size > 0, "Test server write failed.");
            offset += static_cast<std::size_t>(size);

        }

    }

    static protocol::packet read_packet(int fd, protocol::session& connection) {

        std::array<std::uint8_t, 137> buffer{};

        while (true) {

            if (auto pkt = connection.next_packet())
                return std::move(*pkt);

            const ssize_t size = ::recv(fd, buffer.data(), buffer.size(), 0);
            require(size > 0, "Test server read failed.");
            connection.feed(buffer.data(), static_cast<std::size_t>(size));

        }

    }

    static void handshake(int fd, protocol::session& connection) {

        connection.start();
        auto initial = read_packet(fd, connection);
        require(initial.has_flag(protocol::ProtocolFlag::QRY), "Missing initial handshake.");
        auto response = connection.next_response();
        require(response.has_value(), "Missing acceptance.");

        write_all(fd, connection.send(*response));
        auto terminal = read_packet(fd, connection);
        require(terminal.has_flag(protocol::ProtocolFlag::FIN), "Missing terminal FIN.");
        require(connection.get_state() == protocol::SessionState::Established, "Peer handshake incomplete.");

    }

};

static void test_stream_restart_and_control(bool optimized) {

    utils::test_server server([&](int fd) {

        protocol::session connection;
        utils::handshake(fd, connection);
        utils::require(connection.is_optimized() == optimized, "Negotiated framing mismatch.");

        for (int stream = 0; stream < 2; stream++) {

            auto config = utils::read_packet(fd, connection);
            utils::require(
                config.payload_flags == static_cast<std::uint8_t>(protocol::VideoFlag::CONFIG),
                "CONFIG order lost."
            );

            auto parsed_config = protocol::video_payload::parse_config(config.payload);
            utils::require(
                parsed_config.csd0 == std::vector<std::uint8_t>{0x67} &&
                    parsed_config.csd1 == std::vector<std::uint8_t>{0x68},

                "CSD bytes changed."
            );
            auto first = utils::read_packet(fd, connection);
            auto first_frame = protocol::video_payload::parse_frame(first.payload);

            utils::require(
                first.payload_flags == static_cast<std::uint8_t>(protocol::VideoFlag::KEYFRAME) && first_frame.pts_us == 0 &&
                    first_frame.data == std::vector<std::uint8_t>{1, 2, 3, 4},
                "Partial keyframe assembly or timeline reset failed."
            );

            auto second = protocol::video_payload::parse_frame(utils::read_packet(fd, connection).payload);
            utils::require(
                second.pts_us == 33333 && second.data == std::vector<std::uint8_t>{5, 6},
                "Frame data or relative PTS changed."
            );

            auto eos = utils::read_packet(fd, connection);
            utils::require(eos.payload_flags == static_cast<std::uint8_t>(protocol::VideoFlag::EOS), "EOS order lost.");

        }

        auto outgoing = utils::read_packet(fd, connection);
        utils::require(
            outgoing.type == protocol::PacketType::Message && outgoing.payload == std::vector<std::uint8_t>{42},
            "Control packet was lost."
        );

        protocol::packet incoming;
        incoming.type = protocol::PacketType::Message;
        incoming.set_flag(protocol::ProtocolFlag::NACK);
        incoming.set_flag(protocol::ProtocolFlag::OPTS, connection.is_optimized());
        incoming.payload = {99};

        utils::write_all(fd, connection.send(incoming));
        // Wait for client close so the incoming event isn't discarded by disconnect.
        std::uint8_t byte;
        (void)::recv(fd, &byte, 1, 0);

    });

    android::native_transport transport;
    transport.connect("127.0.0.1", server.port, optimized);

    for (int stream = 0; stream < 2; stream++) {

        transport.video_config(protocol::video_payload::make_config({protocol::VideoCodec::H264, {0x67}, {0x68}}));
        const std::array<std::uint8_t, 2> ignored{8, 9};
        utils::require(!transport.video_fragment(ignored, 1000, false, false), "Pre-keyframe wasn't dropped.");
        std::array<std::uint8_t, 2> first{1, 2};
        const std::array<std::uint8_t, 2> tail{3, 4};

        utils::require(!transport.video_fragment(first, 2000, true, true), "Partial unit was sent early.");
        first.fill(0); // Source memory can be reused immediately after submission.
        utils::require(transport.video_fragment(tail, 2000, false, false), "Completed frame wasn't queued.");
        const std::array<std::uint8_t, 2> second{5, 6};
        transport.video_fragment(second, 35333, false, false);
        transport.video_eos();

    }

    protocol::packet message;
    message.type = protocol::PacketType::Message;
    message.set_flag(protocol::ProtocolFlag::NACK);
    message.payload = {42};
    transport.send_packet(std::move(message));
    std::optional<protocol::packet> incoming;

    for (int i = 0; i < 10 && !incoming; i++)
        incoming = transport.wait_packet(500);
    utils::require(
        incoming && incoming->payload == std::vector<std::uint8_t>{99},

        "Persistent reader did not dispatch incoming data."
    );
    transport.close();
    transport.close();
    server.finish();

}

static void test_cancel_handshake() {

    std::atomic<bool> accepted{false};

    utils::test_server server([&](int fd) {

        accepted = true;
        std::array<std::uint8_t, 64> buffer{};

        while (::recv(fd, buffer.data(), buffer.size(), 0) > 0) {
        }

    });

    android::native_transport transport;
    std::atomic<bool> rejected{false};

    std::thread connector([&] {

        try {

            transport.connect("127.0.0.1", server.port, true);

        } catch (const std::exception&) {

            rejected = true;

        }

    });

    for (int i = 0; i < 100 && !accepted; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    transport.interrupt();
    connector.join();
    transport.close();

    utils::require(rejected, "Cancelled handshake unexpectedly succeeded.");
    server.finish();

}

static void test_bounded_queue() {

    std::atomic<bool> release{false};
    std::atomic<bool> verified{false};

    utils::test_server server([&](int fd) {

        protocol::session connection;
        utils::handshake(fd, connection);
        // Hold the reader beyond the per-send timeout. Recovery must preserve
        // both the socket and the complete packet already being written.
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

        bool configured = false;
        bool control_received = false;
        bool recovered_keyframe = false;

        bool recovered_delta = false;
        std::optional<std::uint64_t> previous_pts;

        while (true) {

            auto pkt = utils::read_packet(fd, connection);

            if (pkt.type == protocol::PacketType::Message) {

                utils::require(pkt.payload == std::vector<std::uint8_t>{42}, "Control payload changed during recovery.");
                control_received = true;
                continue;

            }

            if (pkt.payload_flags == static_cast<std::uint8_t>(protocol::VideoFlag::CONFIG)) {

                configured = true;
                continue;

            }

            if (pkt.payload_flags == static_cast<std::uint8_t>(protocol::VideoFlag::EOS))
                break;

            utils::require(configured, "Recovery removed CONFIG.");
            const auto frame = protocol::video_payload::parse_frame(pkt.payload);
            const bool keyframe = pkt.payload_flags == static_cast<std::uint8_t>(protocol::VideoFlag::KEYFRAME);

            if (!previous_pts || frame.pts_us != *previous_pts + 33333)
                utils::require(keyframe, "Dependent frame crossed a dropped-frame gap.");

            previous_pts = frame.pts_us;

            if (frame.data[0] == 201) {

                utils::require(
                    keyframe && frame.pts_us == 200 * 33333ULL,
                    "Recovery keyframe or original PTS timeline was lost."
                );
                recovered_keyframe = true;

            }

            if (frame.data[0] == 202) {

                utils::require(recovered_keyframe, "Delta frame preceded recovery keyframe.");
                recovered_delta = true;

            }

        }

        utils::require(
            configured && control_received && recovered_keyframe && recovered_delta,
            "Recovery lost a boundary, control packet or fresh footage."
        );
        verified = true;

        std::uint8_t byte;
        (void)::recv(fd, &byte, 1, 0);

    });

    android::native_transport transport;
    transport.connect("127.0.0.1", server.port, true);
    transport.video_config(protocol::video_payload::make_config({protocol::VideoCodec::H264, {0x67}, {0x68}}));
    std::vector<std::uint8_t> frame(1024 * 1024, 1);
    bool dropped = false;

    for (int i = 0; i < 100; i++) {

        frame[0] = static_cast<std::uint8_t>(i);

        if (!transport.video_fragment(frame, i * 33333ULL, i == 0, false))
            dropped = true;

    }

    const bool keyframe_requested = transport.needs_video_keyframe();
    protocol::packet control;
    control.type = protocol::PacketType::Message;
    control.set_flag(protocol::ProtocolFlag::NACK);
    control.payload = {42};

    transport.send_packet(std::move(control));
    const std::array<std::uint8_t, 1> key{201};
    const std::array<std::uint8_t, 1> delta{202};
    const bool key_accepted = transport.video_fragment(key, 200 * 33333ULL, true, false);
    const bool request_cleared = !transport.needs_video_keyframe();

    transport.video_fragment(delta, 201 * 33333ULL, false, false);
    transport.video_eos();
    std::this_thread::sleep_for(std::chrono::seconds(6));
    const bool stayed_connected = transport.is_connected();
    release = true;

    for (int i = 0; i < 1000 && !verified && transport.is_connected(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    transport.close();
    server.finish();

    utils::require(
        dropped && keyframe_requested && key_accepted && request_cleared && stayed_connected,
        "Queue overload did not recover on the same connection."
    );
    utils::require(verified, "Recovered stream was not received.");

}

static void test_rejected_handshake() {

    utils::test_server server([](int fd) {

        protocol::session connection;
        connection.start();
        (void)utils::read_packet(fd, connection);
        protocol::packet rejection;
        rejection.type = protocol::PacketType::Communication;

        rejection.set_flag(protocol::ProtocolFlag::ACK);
        rejection.set_flag(protocol::ProtocolFlag::ERR);
        rejection.payload = {1, 'n', 'o'};
        utils::write_all(fd, connection.send(rejection));
        std::uint8_t byte;
        (void)::recv(fd, &byte, 1, 0);

    });

    android::native_transport transport;
    bool rejected = false;

    try {

        transport.connect("127.0.0.1", server.port, true);

    } catch (const std::runtime_error&) {

        rejected = true;

    }

    transport.close();
    server.finish();
    utils::require(rejected, "Rejected handshake unexpectedly succeeded.");

}

int main() {

    try {

        test_stream_restart_and_control(false);
        test_stream_restart_and_control(true);
        test_cancel_handshake();
        test_bounded_queue();
        test_rejected_handshake();

        std::cout
            << "Native transport checks passed: framing, partial frames, PTS, restart, duplex, cancellation, queue bounds.\n";

        return 0;

    } catch (const std::exception& error) {

        std::cerr << error.what() << '\n';

        return 1;

    }

}
