#ifndef CAMDROID_ANDROID_NATIVE_TRANSPORT_H
#define CAMDROID_ANDROID_NATIVE_TRANSPORT_H
#pragma once

#include "session.h"
#include "video_datagram.h"
#include <netinet/in.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

#include <span>
#include <string>
#include <thread>
#include <chrono>

namespace camdroid::android {

    class native_transport {

        private:
        struct outgoing {

            protocol::packet pkt;
            bool terminal_fin = false;
            bool close_after = false;
            std::chrono::steady_clock::time_point queued = std::chrono::steady_clock::now();
            std::optional<std::uint16_t> rotation;

        };

        protocol::session connection;

        std::mutex session_mutex;
        std::mutex queue_mutex;
        std::mutex video_mutex;
        std::mutex thread_mutex;
        std::condition_variable changed;

        std::deque<outgoing> outgoing_packets;
        std::deque<protocol::packet> incoming_packets;
        std::size_t outgoing_bytes = 0;
        std::size_t incoming_bytes = 0;

        std::atomic<int> socket_fd{-1};
        int video_fd = -1;
        sockaddr_in video_address{};
        protocol::video_datagram::token_type video_token{};
        bool video_ready = false;   // Protected by queue_mutex; immutable once enabled.
        std::uint64_t frame_id = 0; // Writer only.

        std::atomic<bool> stopped{false};
        std::atomic<bool> established{false};
        std::atomic<bool> keyframe_needed{false};
        std::atomic<int> requested_rotation{-1};

        bool recovering_video = false; // Protected by queue_mutex.
        bool started = false;
        std::string failure;

        std::thread reader;
        std::thread writer;

        bool video_configured = false;
        bool awaiting_keyframe = true;
        bool pending_keyframe = false;
        std::optional<std::uint64_t> first_pts;
        std::uint64_t pending_pts = 0;
        std::vector<std::uint8_t> pending_frame;

        void stop(const std::string& reason);
        bool enqueue(outgoing item);
        void process_buffered(); // Caller owns session_mutex.
        void read_loop() noexcept;
        void write_loop() noexcept;

        public:
        native_transport() = default;
        ~native_transport();
        native_transport(const native_transport&) = delete;
        native_transport& operator=(const native_transport&) = delete;

        void connect(const std::string& host, int port, bool optimized, std::uint64_t network = 0);
        void close();
        void interrupt();
        bool is_connected() const noexcept;
        bool needs_video_keyframe() const noexcept;

        std::string error();
        void send_packet(protocol::packet pkt);
        void video_config(protocol::packet pkt);
        bool video_fragment(std::span<const std::uint8_t> data, std::uint64_t pts, bool keyframe, bool partial);
        void video_eos();
        void video_rotation(int rotation);

        void video_settings(int fps, int bitrate, bool user_configured = false);
        std::optional<protocol::packet> wait_packet(int timeout_ms);

    };

};

#endif
