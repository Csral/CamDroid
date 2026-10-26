#ifndef CAMDROID_APPLICATION_H
#define CAMDROID_APPLICATION_H
#pragma once

#include "video_payload.h"
#include "video_control.h"
#include "video_decoder.h"
#include <optional>

#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>

#include <string>

struct AVFrame;

namespace camdroid {

    struct application_config {

        std::uint16_t port = 6969;
        int handshake_timeout_ms = 10000;
        bool keep_listening = true;
        std::string decoder = "auto";
        bool automatic_decoder = false;

    };

    struct application_callbacks {

        std::function<void(std::uint16_t)> on_listening;
        std::function<void()> on_connected;
        std::function<void()> on_established;
        std::function<void(const std::string&)> on_negotiation;
        std::function<void(protocol::video_control)> on_video_request_result;

        std::function<void(const protocol::camera_capabilities&)> on_camera_capabilities;
        std::function<void(protocol::capture_selection)> on_capture_selection;

        std::function<void(const std::string&)> on_error;
        std::function<void(std::size_t)> on_received;
        std::function<void(std::size_t)> on_encoded_frame;
        std::function<void(protocol::video_settings)> on_video_settings;
        std::function<void(const protocol::video_config&)> on_video_config;

        std::function<void(const decoder_selection&)> on_decoder_selection;
        std::function<void(double, double)> on_video_timing;
        // Decoded pixels retain their encoded layout. camdroid.rotation (clockwise
        // degrees) and camdroid.mirrored (0/1) in AVFrame metadata describe display.
        std::function<void(const AVFrame&, std::uint64_t)> on_frame;

        std::function<void(std::uint64_t)> on_video_ended;
        std::function<void(std::uint64_t)> on_disconnected;

    };

    // Forward-decl.
    class tcp_socket;

    class application {

        private:
        application_config configuration;
        application_callbacks callbacks;

        std::atomic<bool> started = false;
        std::atomic<bool> stop_requested = false;
        std::atomic<bool> peer_cancelled = false;

        bool cancelled() const noexcept {
            return stop_requested || peer_cancelled;
        }

        std::mutex socket_mutex;
        std::shared_ptr<tcp_socket> active_socket;
        std::mutex control_mutex;
        std::optional<protocol::video_control> requested_control;
        std::uint32_t next_request = 0;
        bool flip_requested = false;

        std::mutex decoder_mutex;
        std::string decoder_preference;
        bool automatic_decoder = false;
        std::uint64_t decoder_identification = 0;

        void run_connection(const std::shared_ptr<tcp_socket>& phone);

        public:
        explicit application(application_config config, application_callbacks handlers = {});

        // Socket callbacks run on the receiver. video callbacks run on the decoder.
        // References are borrowed for the duration of a callback. Each connection
        // is joined and cleared before the listener accepts another peer.
        void run();

        void request_stop() noexcept;
        void disconnect_peer() noexcept;
        void request_video(protocol::video_format format, std::uint32_t bitrate = 0);
        void request_decoder(const std::string& id);
        void identify_decoder();

        void flip_camera();
        void set_automatic_decoder(bool enabled);

    };

};

#endif