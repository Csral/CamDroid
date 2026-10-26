#include "packet_builder.h"
#include <stdexcept>

//TODO: Optimize ts.
namespace camdroid::protocol {

    packet_builder::packet_builder(std::uint8_t version) {
        pkt.version = version;
    }

    packet_builder::packet_builder(std::uint8_t version, const packet& existing) {
        pkt = existing;
        pkt.version = version;
    }

    packet_builder::packet_builder(std::uint8_t version, const packet& existing, std::span<const std::uint8_t> payload) {
        pkt = existing;
        pkt.version = version;
        this->payload(payload);
    }

    packet_builder& packet_builder::version(std::uint8_t value) {
        pkt.version = value;

        return *this;
    }

    packet_builder& packet_builder::flags(std::uint8_t value) {
        pkt.flags = value;

        return *this;
    }

    packet_builder& packet_builder::flag(ProtocolFlag value, bool enabled) {
        pkt.set_flag(value, enabled);

        return *this;
    }

    packet_builder& packet_builder::type(PacketType value) {
        pkt.type = value;

        return *this;
    }

    packet_builder& packet_builder::payload_flags(std::uint8_t value) {
        pkt.payload_flags = value;

        return *this;
    }

    packet_builder& packet_builder::timestamp(std::uint64_t value) {
        pkt.timestamp_ms = value;

        return *this;
    }

    packet_builder& packet_builder::extensions(std::span<const std::uint8_t> value) {
        pkt.extensions.assign(value.begin(), value.end());

        return *this;
    }

    packet_builder& packet_builder::payload(std::span<const std::uint8_t> value) {
        pkt.payload.assign(value.begin(), value.end());

        return *this;
    }

    packet packet_builder::build() const {

        if (pkt.payload_flags > 0x0F)
            throw std::invalid_argument("Packet builder failure: Payload flags exceed 4 bits.");

        if (static_cast<std::uint8_t>(pkt.type) > 0x0F)
            throw std::invalid_argument("Packet builder failure: Packet type exceeds 4 bits.");

        if (pkt.has_flag(ProtocolFlag::OPTS) && !pkt.extensions.empty())
            throw std::invalid_argument("Packet builder failure: OPTS packets cannot contain extensions.");

        return pkt;

    }

}