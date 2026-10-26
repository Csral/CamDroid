#ifndef CAMDROID_PROTOCOL_PACKET_H
#define CAMDROID_PROTOCOL_PACKET_H
#pragma once

#include "protocol.h"

#include <cstdint>
#include <vector>

namespace camdroid::protocol {

    struct packet {

        // Protocol header
        std::uint8_t version = camdroid::protocol::version;
        std::uint8_t flags = 0;

        // Payload header
        PacketType type = PacketType::Message;
        std::uint8_t payload_flags = 0;

        // Timing
        std::uint64_t timestamp_ms = 0;

        // Local UDP reassembly metadata. Never serialized into the protocol header.
        std::uint64_t video_frame_id = 0;

        // Optional additional data
        std::vector<std::uint8_t> extensions;

        // Actual application payload
        std::vector<std::uint8_t> payload;

        [[nodiscard]]
        bool has_flag(ProtocolFlag flag) const noexcept {
            return (flags & static_cast<std::uint8_t>(flag)) != 0;
        }

        void set_flag(ProtocolFlag flag, bool enabled = true) noexcept {

            const auto mask = static_cast<std::uint8_t>(flag);

            if (enabled)
                flags |= mask;
            else
                flags &= static_cast<std::uint8_t>(~mask);

        }

        [[nodiscard]]
        std::uint64_t payload_size() const noexcept {
            return static_cast<std::uint64_t>(payload.size());
        }

        [[nodiscard]]
        std::uint32_t offset() const noexcept {
            return static_cast<std::uint32_t>(extensions.size());
        }

    };

};

#endif