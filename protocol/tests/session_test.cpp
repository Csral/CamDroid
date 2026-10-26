#include "session.h"
#include "video_control.h"
#include "video_payload.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace camdroid::protocol;

namespace camdroid::protocol::tests::utils {

    static void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    static packet handshake(std::uint8_t flags, std::vector<std::uint8_t> payload) {

        packet p;
        p.type = PacketType::Communication;
        p.flags = flags;
        p.payload = std::move(payload);

        return p;

    }

    // Model ordered partial writes / arbitrary receive chunks without socket timing.
    static std::vector<packet> deliver(session& receiver, const std::vector<std::uint8_t>& bytes, std::size_t chunk_size) {

        std::vector<packet> received;

        for (std::size_t offset = 0; offset < bytes.size();) {

            const auto count = std::min(chunk_size, bytes.size() - offset);
            receiver.feed(bytes.data() + offset, count);
            offset += count;

            while (auto p = receiver.next_packet())
                received.push_back(std::move(*p));

        }

        return received;

    }

    static packet take_response(session& sender) {

        auto response = sender.next_response();
        require(response.has_value(), "Expected a queued handshake response");
        require(!sender.next_response(), "Unexpected extra handshake response");

        return std::move(*response);

    }

    static void video_roundtrip(bool optimized, bool negotiate_version, std::size_t chunk_size) {

        session initiator, responder;
        initiator.start();
        responder.start();
        require(!initiator.next_response(), "New session response queue should be empty");

        auto initial = handshake(4, {1, 1, 1, static_cast<std::uint8_t>(optimized ? 128 : 0)});
        // Exercise negotiation back to v1 without changing the production whitelist.
        if (negotiate_version)
            initial.version = 2;

        deliver(responder, initiator.send(initial), chunk_size);
        session* terminal_sender = &initiator;
        session* terminal_receiver = &responder;

        if (negotiate_version) {

            deliver(initiator, responder.send(take_response(responder)), chunk_size);
            deliver(responder, initiator.send(take_response(initiator)), chunk_size);

        }

        deliver(*terminal_sender, terminal_receiver->send(take_response(*terminal_receiver)), chunk_size);
        require(terminal_sender->get_state() == SessionState::Finalizing, "Expected terminal FIN write pending");
        require(!terminal_sender->is_optimized(), "Optimization activated before FIN write completion");

        const auto terminal_bytes = terminal_sender->send(take_response(*terminal_sender));
        deliver(*terminal_receiver, terminal_bytes, chunk_size);
        require(terminal_receiver->get_state() == SessionState::Established, "Terminal FIN receiver did not establish");

        packet video = video_payload::make_frame({0, {0, 1, 2, 0xFF}}, true);
        video.set_flag(ProtocolFlag::NACK);
        video.set_flag(ProtocolFlag::OPTS, optimized);
        video.timestamp_ms = 123456789;
        const auto early = terminal_receiver->send(video);

        terminal_sender->feed(early.data(), early.size());
        require(!terminal_sender->next_packet(), "Early video should wait for local FIN completion");
        terminal_sender->on_terminal_fin_sent();
        require(terminal_sender->is_optimized() == optimized, "Initiator lost negotiated optimization state");
        auto received = terminal_sender->next_packet();
        require(received && received->payload == video.payload, "Buffered early video lost");

        for (const auto status : {0, 1, 2, 3}) {

            video_control control{
                static_cast<std::uint32_t>(status + 1),
                {1920, 1080, 60},
                static_cast<std::uint8_t>(status)
            };

            auto query = make_video_control(control);
            query.set_flag(ProtocolFlag::OPTS, optimized);
            const auto requests = deliver(*terminal_receiver, terminal_sender->send(query), chunk_size);
            require(
                requests.size() == 1 && parse_video_control(requests[0]).format.fps == 60,

                "Format query was not delivered"
            );
            auto reply = make_video_control(control, true);
            reply.set_flag(ProtocolFlag::OPTS, optimized);
            const auto replies = deliver(*terminal_sender, terminal_receiver->send(reply), chunk_size);

            require(
                replies.size() == 1 && parse_video_control(replies[0]).status == status,
                "Format response was not delivered"
            );

            require(
                terminal_sender->get_state() == SessionState::Established &&
                    terminal_receiver->get_state() == SessionState::Established,
                "Format negotiation changed the connection state"
            );

        }

        std::vector<std::uint8_t> stream;

        for (int i = 0; i < 5; ++i) {

            video.timestamp_ms++;
            const auto bytes = terminal_sender->send(video);
            stream.insert(stream.end(), bytes.begin(), bytes.end());

        }

        const auto frames = deliver(*terminal_receiver, stream, chunk_size);
        require(frames.size() == 5, "Incorrect number of reconstructed video packets");

        for (std::size_t i = 0; i < frames.size(); ++i) {

            require(frames[i].type == PacketType::Video && frames[i].payload == video.payload, "Video type or bytes changed");
            require(
                frames[i].payload_flags == static_cast<std::uint8_t>(VideoFlag::KEYFRAME) &&
                    frames[i].timestamp_ms == 123456790 + i,
                "Video metadata changed"
            );

        }

        initiator.close();
        responder.close();
        bool rejected = false;

        try {

            (void)initiator.send(video);

        } catch (const std::runtime_error&) {

            rejected = true;

        }

        require(rejected, "Closed session accepted a send");

    }

    static void invalid_handshakes() {

        for (std::size_t size = 0; size < 4; ++size) {

            session sender, receiver;
            sender.start();
            receiver.start();
            deliver(receiver, sender.send(handshake(4, std::vector<std::uint8_t>(size, 1))), 1);
            require(receiver.get_state() == SessionState::Closing, "Short handshake was not rejected");
            require(take_response(receiver).has_flag(ProtocolFlag::ERR), "Missing rejection response");

        }

        session sender, receiver;
        sender.start();
        receiver.start();
        auto initial = handshake(4, {1, 1, 1, 0});
        initial.version = 2;

        deliver(receiver, sender.send(initial), 1);
        (void)take_response(receiver);
        deliver(receiver, sender.send(handshake(33, {1})), 1);
        require(receiver.get_state() == SessionState::Closing, "FIN bypassed pending negotiation");

    }

}

int main() {

    using namespace camdroid::protocol::tests::utils;

    int failures = 0;

    auto run = [&](const std::string& name, auto test) {

        try {

            test();
            std::cout << "PASS: " << name << '\n';

        } catch (const std::exception& error) {

            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';

        }

    };

    run("invalid handshakes", invalid_handshakes);

    for (bool optimized : {false, true}) {

        for (bool negotiation : {false, true}) {

            for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {

                const auto name = std::string(optimized ? "optimized" : "normal") +
                                  (negotiation ? " negotiated" : " direct") + " chunk=" + std::to_string(chunk);

                run(name, [&] {
                    video_roundtrip(optimized, negotiation, chunk);
                });

            }

        }

    }

    return failures ? 1 : 0;

}