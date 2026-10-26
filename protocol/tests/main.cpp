#include "packet.h"
#include "parser.h"
#include "serializer.h"

#include <cassert>
#include <iostream>

void test_optimized_packet() {

    using namespace camdroid::protocol;

    packet original;

    original.type = PacketType::Video;
    original.timestamp_ms = 987654321;
    original.payload = {0x01, 0x02, 0x03, 0x04};

    original.set_flag(ProtocolFlag::OPTS);
    original.set_flag(ProtocolFlag::NACK);

    auto bytes = serializer::serialize(original);

    assert(bytes.size() == OPTS_HEADER_SIZE + original.payload.size());

    parser receiver;
    receiver.set_optimized(true);

    receiver.feed(bytes.data(), bytes.size());

    auto received = receiver.next_packet();

    assert(received.has_value());
    assert(received->type == PacketType::Video);
    assert(received->has_flag(ProtocolFlag::OPTS));
    assert(received->has_flag(ProtocolFlag::NACK));
    assert(received->payload == original.payload);

    std::cout << "Optimized packet round-trip successful.\n";

}

void test_framing_transition() {

    using namespace camdroid::protocol;

    packet acknowledgement;
    acknowledgement.type = PacketType::Communication;
    acknowledgement.set_flag(ProtocolFlag::ACK);
    acknowledgement.payload = {'O', 'P', 'T', 'S', 'Y'};

    packet video;
    video.type = PacketType::Video;
    video.set_flag(ProtocolFlag::OPTS);
    video.set_flag(ProtocolFlag::NACK);
    video.payload = {0xAA, 0xBB, 0xCC};

    auto normal_bytes = serializer::serialize(acknowledgement);
    auto optimized_bytes = serializer::serialize(video);

    std::vector<std::uint8_t> stream;

    stream.insert(stream.end(), normal_bytes.begin(), normal_bytes.end());

    stream.insert(stream.end(), optimized_bytes.begin(), optimized_bytes.end());

    parser receiver;

    // Simulate one TCP receive containing both packets.
    receiver.feed(stream.data(), stream.size());

    auto first = receiver.next_packet();

    assert(first.has_value());
    assert(first->type == PacketType::Communication);
    assert(first->has_flag(ProtocolFlag::ACK));

    // Negotiation has completed.
    receiver.set_optimized(true);

    auto second = receiver.next_packet();

    assert(second.has_value());
    assert(second->type == PacketType::Video);
    assert(second->has_flag(ProtocolFlag::OPTS));
    assert(second->payload == video.payload);

    std::cout << "Framing transition successful.\n";

}

int main() {

    using namespace camdroid::protocol;

    packet original;

    original.type = PacketType::Message;
    original.timestamp_ms = 123456789;
    original.payload = {'H', 'e', 'l', 'l', 'o'};
    original.extensions = {0x12, 0x34};

    auto bytes = serializer::serialize(original);

    parser receiver;

    // Deliberately split the packet into two fragments.
    receiver.feed(bytes.data(), 10);

    assert(!receiver.next_packet().has_value());

    receiver.feed(bytes.data() + 10, bytes.size() - 10);

    auto received = receiver.next_packet();

    assert(received.has_value());
    assert(received->payload == original.payload);
    assert(received->extensions == original.extensions);
    assert(received->timestamp_ms == original.timestamp_ms);

    test_optimized_packet();
    test_framing_transition();

    std::cout << "Protocol round-trip successful.\n";

    return 0;

}