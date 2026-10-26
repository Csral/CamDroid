#ifndef CAMDROID_PROTOCOL_PACKET_BUILDER_H
#define CAMDROID_PROTOCOL_PACKET_BUILDER_H
#pragma once

#include "packet.h"

#include <cstdint>
#include <span>
#include <vector>

namespace camdroid::protocol {

    class packet_builder {

        private:
        packet pkt;

        public:
        explicit packet_builder(std::uint8_t version = protocol::version);
        packet_builder(std::uint8_t version, const packet& existing);
        packet_builder(std::uint8_t version, const packet& existing, std::span<const std::uint8_t> payload);

        packet_builder& version(std::uint8_t value);
        packet_builder& flags(std::uint8_t value);
        packet_builder& flag(ProtocolFlag value, bool enabled = true);

        packet_builder& type(PacketType value);
        packet_builder& payload_flags(std::uint8_t value);

        packet_builder& timestamp(std::uint64_t value);

        packet_builder& extensions(std::span<const std::uint8_t> value);
        packet_builder& payload(std::span<const std::uint8_t> value);

        [[nodiscard]]
        packet build() const;

    };

};

#endif