#pragma once
#ifndef CAMDROID_PROTOCOL_VIDEO_DATAGRAM_H
#define CAMDROID_PROTOCOL_VIDEO_DATAGRAM_H

#include "packet.h"
#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>

namespace camdroid::protocol::video_datagram {

    constexpr std::uint8_t setup = 0x20;
    constexpr std::uint8_t frame = 0x21;
    constexpr std::uint8_t recover = 0x22;
    constexpr std::size_t header_size = 36;
    constexpr std::size_t fragment_size = 1164;

    constexpr std::size_t max_frame = 80 * 1024 * 1024;
    using token_type = std::array<std::uint8_t, 16>;

    inline void put(std::span<std::uint8_t> bytes, std::uint64_t value) {

        for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {

            *it = static_cast<std::uint8_t>(value);
            value >>= 8;

        }

    }

    inline std::uint64_t get(std::span<const std::uint8_t> bytes) {

        std::uint64_t value = 0;

        for (const auto byte : bytes)
            value = (value << 8) | byte;

        return value;

    }

    inline packet control(std::uint8_t opcode, std::size_t length) {

        packet pkt;
        pkt.type = PacketType::Communication;
        pkt.payload.resize(length);
        pkt.payload[0] = opcode;

        return pkt;

    }

    inline void validate(const packet& pkt) {

        const auto data = std::span<const std::uint8_t>(pkt.payload);
        const auto opcode = data.empty() ? 0 : data[0];
        bool valid = false;

        if (opcode == setup && data.size() == 19)
            valid = get(data.subspan(1, 2)) != 0;

        if (opcode == frame && data.size() == 14)
            valid = get(data.subspan(1, 8)) != 0 && get(data.subspan(9, 4)) >= 9 && get(data.subspan(9, 4)) <= max_frame &&
                    (data[13] == 0 || data[13] == 2);

        if (opcode == recover && data.size() == 1)
            valid = true;

        const auto allowed = static_cast<std::uint8_t>(ProtocolFlag::OPTS);

        if (!valid || pkt.type != PacketType::Communication || pkt.payload_flags || !pkt.extensions.empty() ||
            (pkt.flags & ~allowed))
            throw std::invalid_argument("Invalid UDP video control packet.");

    }

}

#endif