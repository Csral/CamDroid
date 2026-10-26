#ifndef CAMDROID_PROTOCOL_SESSION_H
#define CAMDROID_PROTOCOL_SESSION_H
#pragma once

#include "packet.h"
#include "parser.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include <deque>

#include <string>

namespace camdroid::protocol {

    enum class SessionState : std::uint8_t {

        Disconnected,
        Handshaking,
        Established,
        Finalizing,
        Closing,
        Closed

    };

    enum class VideoState : std::uint8_t {

        Unconfigured,
        AwaitingKeyframe,
        Streaming,
        Ended

    };

    enum class CommunicationOpcode : std::uint8_t {

        Handshake = 0x01,
        Optimization = 0x02

    };

    class dispatcher {
        // Owns queues and worker threads.
        // Routes validated packets by PacketType.
    };

    class session {

        private:
        SessionState state = SessionState::Disconnected;

        parser receiver;

        bool incoming_optimized = false;
        bool outgoing_optimized = false;

        bool peer_supports_optimization = false;
        bool optimization_pending = false;

        std::uint8_t negotiated_version = camdroid::protocol::version;
        std::uint8_t pending_version = camdroid::protocol::version;
        VideoState video_state = VideoState::Unconfigured;

        std::uint8_t pending_negotiations = 0;
        std::uint8_t pending_counteroffers = 0;

        // Internal validation and negotiation
        void validate_packet(const packet& pkt);
        void handle_communication(const packet& pkt);
        void handle_handshake(const packet& pkt);

        void handle_video(const packet& pkt);

        packet make_handshake_response(std::uint8_t selected_version, bool accepted);

        void reject_handshake(const std::string& message);

        std::deque<packet> outgoing_responses;
        std::string last_error;
        bool terminal_fin_pending = false;

        public:
        session();

        // Called after transport establishes a connection.
        void start();

        // Called when transport receives bytes.
        void feed(const void* data, std::size_t size);

        // Extract and process a received packet.
        [[nodiscard]]
        std::optional<packet> next_packet();

        // Encode an outgoing packet.
        [[nodiscard]]
        std::vector<std::uint8_t> send(const packet& pkt);

        [[nodiscard]]
        SessionState get_state() const noexcept;

        [[nodiscard]]
        bool is_optimized() const noexcept;

        [[nodiscard]]
        std::optional<packet> next_response();

        [[nodiscard]]
        bool is_terminal_fin_pending() const noexcept;

        [[nodiscard]] bool has_buffered_data() const noexcept {
            return receiver.has_buffered_data();
        }

        void close();

        // Call only after all terminal FIN bytes have been written to the transport.
        void on_terminal_fin_sent();

    };

};

#endif