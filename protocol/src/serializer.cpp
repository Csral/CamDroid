#include "serializer.h"
#include "utils.h"

#include <limits>
#include <stdexcept>

namespace camdroid::protocol {

    std::vector<std::uint8_t> serializer::serialize(const packet& pkt) {

        bool optimized = pkt.has_flag(ProtocolFlag::OPTS);

        if (optimized && !pkt.extensions.empty())
            throw std::invalid_argument("Protocol serialization failure: OPTS packets cannot contain extension data.");

        if (pkt.payload_flags > 0x0F)
            throw std::invalid_argument("Protocol serialization failure: Payload flags exceed 4 bits.");

        std::uint8_t type = static_cast<std::uint8_t>(pkt.type);

        if (type > 0x0F)
            throw std::invalid_argument("Protocol serialization failure: Packet type exceeds 4 bits.");

        if (pkt.extensions.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            throw std::length_error("Protocol serialization failure: Extension size exceeds permitted offset.");

        std::uint64_t payload_size = static_cast<std::uint64_t>(pkt.payload.size());
        std::uint64_t extension_size = static_cast<std::uint64_t>(pkt.extensions.size());

        std::size_t header_size = optimized ? OPTS_HEADER_SIZE : NORMAL_HEADER_SIZE;

        if (pkt.extensions.size() > std::numeric_limits<std::size_t>::max() - header_size)
            throw std::length_error("Protocol serialization failure: Packet size overflow.");

        std::size_t total_size = header_size + pkt.extensions.size();

        if (pkt.payload.size() > std::numeric_limits<std::size_t>::max() - total_size)
            throw std::length_error("Protocol serialization failure: Packet size overflow.");

        total_size += pkt.payload.size();

        std::vector<std::uint8_t> buffer;
        buffer.reserve(total_size);

        // Protocol header
        if (!optimized) {

            buffer.insert(buffer.end(), MAGIC, MAGIC + 3);

        }

        buffer.push_back(pkt.flags);

        if (!optimized)
            buffer.push_back(pkt.version);

        // Payload header
        buffer.push_back(static_cast<std::uint8_t>(type << 4));
        buffer.push_back(static_cast<std::uint8_t>(pkt.payload_flags << 4));
        buffer.push_back(0);

        // Timestamp and size
        utils::write_u64(buffer, pkt.timestamp_ms);
        utils::write_u64(buffer, payload_size);

        // Optional extension offset and data
        if (!optimized) {

            utils::write_u32(buffer, static_cast<std::uint32_t>(extension_size));

            buffer.insert(buffer.end(), pkt.extensions.begin(), pkt.extensions.end());

        }

        // Actual payload
        buffer.insert(buffer.end(), pkt.payload.begin(), pkt.payload.end());

        return buffer;

    }

}