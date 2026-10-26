#include "parser.h"
#include "utils.h"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace camdroid::protocol {

    parser::parser(std::uint64_t max_payload_size, std::uint32_t max_extension_size)
        : max_payload_size(max_payload_size), max_extension_size(max_extension_size) {
        return;
    }

    void parser::feed(const void* data, std::size_t size) {

        if (size == 0)
            return;

        if (data == nullptr)
            throw std::invalid_argument("Protocol parsing failure: Null input buffer.");

        if (size > buffer.max_size() - buffer.size())
            throw std::length_error("Protocol parsing failure: Input buffer exceeds maximum size.");

        const auto* bytes = static_cast<const std::uint8_t*>(data);
        buffer.insert(buffer.end(), bytes, bytes + size);

        return;

    }

    std::optional<packet> parser::next_packet(void) {

        std::size_t header_size = optimized ? OPTS_HEADER_SIZE : NORMAL_HEADER_SIZE;

        if (buffer.size() < header_size)
            return std::nullopt;

        std::size_t pos = 0;

        if (!optimized) {

            if (std::memcmp(buffer.data(), MAGIC, 3) != 0)
                throw std::runtime_error("Protocol parsing failure: Invalid magic number.");

            pos += 3;

        }

        packet pkt;

        pkt.flags = buffer[pos++];

        if (optimized != pkt.has_flag(ProtocolFlag::OPTS))
            throw std::runtime_error("Protocol parsing failure: Unexpected framing mode.");

        if (!optimized) {

            pkt.version = buffer[pos++];

        }

        // Payload header
        std::uint8_t type_byte = buffer[pos++];
        std::uint8_t flags_byte = buffer[pos++];
        std::uint8_t reserved_byte = buffer[pos++];

        if ((type_byte & 0x0F) != 0 || (flags_byte & 0x0F) != 0 || reserved_byte != 0)
            throw std::runtime_error("Protocol parsing failure: Reserved bits must be zero.");

        pkt.type = static_cast<PacketType>(type_byte >> 4);
        pkt.payload_flags = flags_byte >> 4;

        pkt.timestamp_ms = utils::read_u64(buffer.data() + pos);
        pos += 8;

        std::uint64_t payload_size = utils::read_u64(buffer.data() + pos);
        pos += 8;

        std::uint32_t extension_size = 0;

        if (!optimized) {

            extension_size = utils::read_u32(buffer.data() + pos);
            pos += 4;

        }

        if (payload_size > max_payload_size)
            throw std::length_error("Protocol parsing failure: Payload exceeds configured limit.");

        if (extension_size > max_extension_size)
            throw std::length_error("Protocol parsing failure: Extensions exceed configured limit.");

        if (payload_size > std::numeric_limits<std::size_t>::max() - header_size - extension_size)
            throw std::length_error("Protocol parsing failure: Packet size overflow.");

        std::size_t total_size = header_size + extension_size + static_cast<std::size_t>(payload_size);

        if (buffer.size() < total_size)
            return std::nullopt;

        // Extension data
        if (extension_size > 0) {

            pkt.extensions.assign(buffer.begin() + header_size, buffer.begin() + header_size + extension_size);

        }

        // Application payload
        std::size_t payload_start = header_size + extension_size;

        pkt.payload.assign(buffer.begin() + payload_start, buffer.begin() + total_size);

        buffer.erase(buffer.begin(), buffer.begin() + total_size);

        return pkt;

    }

    void parser::set_optimized(bool enabled) {
        this->optimized = enabled;

        return;
    }

    void parser::reset(void) {
        buffer.clear();
        optimized = false;

        return;
    }

};