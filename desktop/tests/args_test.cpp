#include "virtual_camera.h"
#include "tcp_socket.h"
#include "arg_parser.h"
#include "application.h"
#include "session.h"

#include "video_payload.h"
#include "utils.h"
#include <atomic>
#include <chrono>
#include <condition_variable>

#include <functional>
#include <mutex>
#include <thread>
extern "C" {
#include <libavutil/frame.h>

#include <libavutil/dict.h>
}

#include <iostream>
#include <sstream>
#include <stdexcept>

namespace camdroid::utils {

    static void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    static void rejects(const std::vector<std::string>& arguments) {

        try {

            (void)argument_parser(arguments);

        } catch (const std::exception&) {

            return;

        }

        throw std::runtime_error("Invalid CLI arguments were accepted.");

    }

    class loopback_peer {

        private:
        #ifdef _WIN32
        SOCKET handle = INVALID_SOCKET;
        #else
        int handle = -1;
        #endif

        public:
        loopback_peer() {

            handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            #ifdef _WIN32
            require(handle != INVALID_SOCKET, "Cannot allocate test socket.");
            const DWORD timeout = 2000;
            ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

            #else
            require(handle >= 0, "Cannot allocate test socket.");
            const timeval timeout{2, 0};
            ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            #endif

        }

        ~loopback_peer() {

            #ifdef _WIN32
            ::closesocket(handle);
            #else
            ::close(handle);
            #endif

        }

        void connect(std::uint16_t port) {

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            require(
                ::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "Test connection failed."
            );

        }

        std::uint16_t reserve() {

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            require(::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Cannot reserve test port.");

            #ifdef _WIN32
            int size = sizeof(address);
            #else
            socklen_t size = sizeof(address);
            #endif
            require(::getsockname(handle, reinterpret_cast<sockaddr*>(&address), &size) == 0, "Cannot query test port.");

            return ntohs(address.sin_port);

        }

        void send(const std::vector<std::uint8_t>& bytes) {

            std::size_t offset = 0;

            while (offset < bytes.size()) {

                #ifdef _WIN32
                const int flags = 0;
                #else
                const int flags = MSG_NOSIGNAL;

                #endif
                const auto count = ::send(
                    handle,
                    reinterpret_cast<const char*>(bytes.data() + offset),
                    static_cast<int>(bytes.size() - offset),

                    flags
                );
                require(count > 0, "Test send failed.");
                offset += count;

            }

        }

        std::vector<std::uint8_t> read(std::size_t size) {

            std::vector<std::uint8_t> bytes(size);
            std::size_t offset = 0;

            while (offset < size) {

                const auto count =
                    ::recv(handle, reinterpret_cast<char*>(bytes.data() + offset), static_cast<int>(size - offset), 0);
                require(count > 0, "Test receive failed.");
                offset += count;

            }

            return bytes;

        }

        void handshake(bool optimized, bool video = false, int control_status = -1) {

            protocol::session session;
            session.start();
            protocol::packet request;
            request.type = protocol::PacketType::Communication;
            request.set_flag(protocol::ProtocolFlag::QRY);

            request.payload = {1, 1, 1, static_cast<std::uint8_t>(optimized ? 128 : 0)};
            send(session.send(request));
            auto bytes = read(28);

            auto tail = read(
                static_cast<std::size_t>(protocol::utils::read_u64(bytes.data() + 16)) +
                protocol::utils::read_u32(bytes.data() + 24)
            );
            bytes.insert(bytes.end(), tail.begin(), tail.end());
            session.feed(bytes.data(), bytes.size());

            while (session.next_packet()) {
            }

            while (auto response = session.next_response()) {

                const bool terminal = session.is_terminal_fin_pending();
                send(session.send(*response));

                if (terminal)
                    session.on_terminal_fin_sent();

            }

            require(session.get_state() == protocol::SessionState::Established, "Test handshake did not establish.");

            if (control_status >= 0) {

                auto header = read(optimized ? 20 : 28);
                const auto size = protocol::utils::read_u64(header.data() + (optimized ? 12 : 16));
                auto payload = read(static_cast<std::size_t>(size));
                header.insert(header.end(), payload.begin(), payload.end());
                session.feed(header.data(), header.size());

                auto query = session.next_packet();
                require(query.has_value(), "Desktop did not send the webcam query.");
                auto control = protocol::parse_video_control(*query);
                require(control.format == protocol::video_format{1920, 1080, 60}, "Wrong requested webcam format.");
                control.status = static_cast<std::uint8_t>(control_status);

                auto response = protocol::make_video_control(control, true);
                response.set_flag(protocol::ProtocolFlag::OPTS, optimized);
                send(session.send(response));

            }

            if (video) {

                protocol::video_config config;

                config.csd0 = {
                    0,
                    0,
                    0,
                    1,

                    0x67,
                    0x42,
                    0xc0,
                    0xa,
                    0xda,

                    0x7b,
                    1,
                    0x10,
                    0,
                    0,

                    3,
                    0,
                    0x10,
                    0,
                    0,

                    3,
                    3,
                    0xc8,
                    0xf1,
                    0x22,

                    0x6a
                };
                config.csd1 = {0, 0, 0, 1, 0x68, 0xce, 0xf, 0xc8};
                config.rotation = 90;
                auto configuration = protocol::video_payload::make_config(config);

                configuration.set_flag(protocol::ProtocolFlag::OPTS, optimized);
                send(session.send(configuration));

                for (int i = 0; i < 80; i++) {

                    const protocol::video_frame frame{
                        static_cast<std::uint64_t>(i) * 33333,
                        {0,
                         0,
                         0,

                         1,
                         0x65,
                         0x88,
                         0x84,
                         0x3a,

                         0x11,
                         0x8a,
                         0,
                         2,
                         0x18,

                         0xf1,
                         0xc0,
                         0,
                         0x40,
                         0xf6,

                         0x38,
                         0,
                         8,
                         0x79,
                         0x60}

                    };
                    auto packet = protocol::video_payload::make_frame(frame, true);
                    packet.set_flag(protocol::ProtocolFlag::OPTS, optimized);
                    send(session.send(packet));

                }

            }

            auto eos = protocol::video_payload::make_eos();
            eos.set_flag(protocol::ProtocolFlag::NACK);
            eos.set_flag(protocol::ProtocolFlag::OPTS, optimized);
            send(session.send(eos));

        }

    };

    static void test_lifecycle() {

        tcp_socket sockets;
        loopback_peer reservation;
        const auto requested = reservation.reserve();
        std::mutex mutex;
        std::condition_variable changed;

        int listening = 0;
        int errors = 0;
        int ended = 0;
        int disconnected = 0;
        int frames = 0;

        std::thread::id receiver_id;
        std::thread::id decoder_id;
        std::uint16_t actual = 0;
        std::exception_ptr failure;
        application_callbacks callbacks;

        callbacks.on_connected = [&] {
            receiver_id = std::this_thread::get_id();
        };

        callbacks.on_frame = [&](const AVFrame& frame, std::uint64_t) {

            decoder_id = std::this_thread::get_id();
            const auto* rotation = av_dict_get(frame.metadata, "camdroid.rotation", nullptr, 0);
            require(
                frame.width == 16 && frame.height == 16 && rotation && std::string(rotation->value) == "90",

                "Decoded frame or orientation is incorrect."
            );
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::lock_guard lock(mutex);
            frames++;

        };

        callbacks.on_listening = [&](std::uint16_t port) {

            std::lock_guard lock(mutex);
            actual = port;
            listening++;
            changed.notify_all();

        };

        callbacks.on_error = [&](const std::string&) {
            std::lock_guard lock(mutex);
            errors++;
            changed.notify_all();
        };

        callbacks.on_video_ended = [&](std::uint64_t) {
            std::lock_guard lock(mutex);
            ended++;
            changed.notify_all();
        };

        callbacks.on_disconnected = [&](std::uint64_t) {
            std::lock_guard lock(mutex);
            disconnected++;
            changed.notify_all();
        };

        application app(application_config{requested, 300}, std::move(callbacks));

        std::jthread server([&](std::stop_token token) {

            std::stop_callback cancellation(token, [&] {
                app.request_stop();
            });

            try {

                app.run();

            } catch (...) {

                std::lock_guard lock(mutex);
                failure = std::current_exception();
                changed.notify_all();

            }

        });

        const auto wait = [&](const std::function<bool()>& condition) {

            std::unique_lock lock(mutex);
            require(
                changed.wait_for(
                    lock,
                    std::chrono::seconds(5),

                    [&] {
                        return failure || condition();
                    }
                ),
                "Lifecycle test timed out."
            );

            if (failure)
                std::rethrow_exception(failure);

        };

        wait([&] {
            return listening == 1;
        });

        require(actual != requested, "Occupied port did not select an alternative.");

        for (int attempt = 0; attempt < 4; attempt++) {

            app.request_video({1920, 1080, 60});

            {

                loopback_peer peer;
                peer.connect(actual);
                peer.handshake(attempt % 2 != 0, true, attempt);

            }

            wait([&] {
                return listening >= attempt + 2 && ended == attempt + 1;
            });

        }

        {

            loopback_peer peer;
            peer.connect(actual);
            peer.send(std::vector<std::uint8_t>(28, 0));

            wait([&] {
                return errors == 1 && listening >= 6;
            });

        }

        {

            loopback_peer peer;
            peer.connect(actual);
            peer.send({'N'});

            wait([&] {
                return errors == 2 && listening >= 7;
            });

        }

        {

            loopback_peer peer;
            peer.connect(actual);
            peer.handshake(false);

            wait([&] {
                return ended == 5;
            });

            peer.send({'N'});

        }

        wait([&] {
            return errors == 3 && listening >= 8;
        });

        {

            loopback_peer peer;
            peer.connect(actual);
            peer.handshake(true);

            wait([&] {
                return ended == 6;
            });

            app.disconnect_peer();

            wait([&] {
                return listening >= 9;
            });

            require(errors == 3, "Intentional disconnect reported an error.");

        }

        require(frames == 320, "Bounded queue lost decoded frames or EOS ordering.");
        require(decoder_id != receiver_id, "Reception and decoding share a thread.");
        const auto start = std::chrono::steady_clock::now();
        app.request_stop();
        server.join();

        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "Listener shutdown blocked.");
        require(disconnected == 8, "A connection was not cleaned up.");
        std::cout << "Lifecycle: reconnect, framing, timeout, fallback and cancellation passed.\n";

    }

};

#ifdef CAMDROID_GUI_TESTS
namespace camdroid {

    int test_gui(int argc, char** argv);

}

#endif

int main(int argc, char** argv) {

    try {

        #ifdef CAMDROID_GUI_TESTS
        if (argc > 1 && std::string(argv[1]) == "--gui-tests")
            return camdroid::test_gui(argc, argv);

        #endif
        const auto defaults = camdroid::argument_parser(std::vector<std::string>{});
        camdroid::utils::require(defaults.port == 6969 && !defaults.preview && !defaults.help, "Incorrect defaults.");
        const auto options = camdroid::argument_parser({"--port", "7000", "--preview"});
        camdroid::utils::require(options.port == 7000 && options.preview, "Options were not parsed.");
        camdroid::utils::require(camdroid::argument_parser({"-p", "4097"}).port == 4097, "Lower boundary failed.");

        camdroid::utils::require(camdroid::argument_parser({"--port=65535"}).port == 65535, "Upper boundary failed.");
        camdroid::utils::require(
            camdroid::argument_parser({"--port", "7000", "-p", "8000"}).port == 8000,
            "Repeated port policy failed."
        );

        camdroid::utils::require(camdroid::argument_parser({"--bad", "--help"}).help, "Help precedence failed.");
        camdroid::utils::require(camdroid::argument_parser({"-h"}).help, "Help alias failed.");

        for (const auto& value :
             {"", "0", "4096", "65536", "-1", "+7000", "7.0", "7000x", " 7000", "99999999999999999999999999999999999999"})
            camdroid::utils::rejects({"--port", value});
        camdroid::utils::rejects({"--port"});
        camdroid::utils::rejects({"--port", "--preview"});

        camdroid::utils::rejects({"--port="});
        camdroid::utils::rejects({"--preview=true"});
        camdroid::utils::rejects({"--unknown"});
        camdroid::utils::rejects({"7000"});

        camdroid::utils::require(camdroid::argument_parser({"--gui"}).gui, "GUI option failed.");
        camdroid::utils::rejects({"--gui", "--preview"});
        camdroid::utils::rejects({"--headless", "--preview"});
        camdroid::utils::rejects({"--headless", "--gui"});
        camdroid::utils::require(camdroid::argument_parser({"--headless"}).headless, "Headless option failed.");

        camdroid::utils::require(!defaults.headless, "GUI must be the default.");
        #ifdef _WIN32

        {

            camdroid::virtual_camera webcam;
            camdroid::utils::require(webcam.app_outputs_allowed(), "Default app-output policy failed.");
            webcam.set_settings({60, 4000000, true});
            camdroid::utils::require(webcam.app_outputs_allowed(), "User capture settings disabled output conversion.");
            webcam.allow_app_outputs(true);

            webcam.set_settings({60, 6000000, true});
            camdroid::utils::require(webcam.app_outputs_allowed(), "Bitrate update overrode the output toggle.");
            webcam.allow_app_outputs(false);
            webcam.set_settings({60, 7000000, true});
            camdroid::utils::require(!webcam.app_outputs_allowed(), "Capture settings reset an explicit output policy.");

            webcam.allow_app_outputs(true);
            webcam.set_settings({30, 6000000, true});
            camdroid::utils::require(webcam.app_outputs_allowed(), "FPS change disabled output conversion.");

        }

        #endif
        camdroid::utils::test_lifecycle();

        std::ostringstream help;
        camdroid::show_help(help);
        camdroid::utils::require(
            help.str().find("--port") != std::string::npos && help.str().find("--preview") != std::string::npos,

            "Help registry is incomplete."
        );
        std::cout << "CLI argument checks passed.\n";

        return 0;

    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

}