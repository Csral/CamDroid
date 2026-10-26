#include "video_control.h"
#include "video_payload.h"
#include "parser.h"
#include "serializer.h"
#include "utils.h"

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace camdroid::protocol;

namespace tests {

    void require(bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    template <typename Function> void rejects(Function function) {

        try {

            function();

        } catch (const std::invalid_argument&) {

            return;

        }

        throw std::runtime_error("Malformed video payload was accepted.");

    }

    void test_wire_packets() {

        video_config config{VideoCodec::H264, {0x67, 0x01}, {0x68}};
        auto bytes = video_payload::serialize_config(config);
        require(bytes == std::vector<std::uint8_t>{1, 0, 0, 0, 2, 0x67, 1, 0, 0, 0, 1, 0x68}, "Incorrect CONFIG wire bytes.");
        auto decoded = video_payload::parse_config(bytes);

        require(
            decoded.codec == config.codec && decoded.csd0 == config.csd0 && decoded.csd1 == config.csd1,
            "CONFIG round trip failed."
        );

        for (const auto& single : {video_config{VideoCodec::H264, {0x67}, {}}, video_config{VideoCodec::H264, {}, {0x68}}}) {

            auto decoded_single = video_payload::parse_config(video_payload::serialize_config(single));
            require(decoded_single.csd0 == single.csd0 && decoded_single.csd1 == single.csd1, "Single CSD buffer failed.");

        }

        for (const auto rotation : {0, 90, 180, 270}) {

            for (const bool mirrored : {false, true}) {

                config.rotation = rotation;
                config.mirrored = mirrored;
                const auto oriented = video_payload::parse_config(video_payload::serialize_config(config));
                require(oriented.rotation == rotation && oriented.mirrored == mirrored, "Orientation round trip failed.");

            }

        }

        config.rotation = 0;
        config.mirrored = false;

        video_frame frame{0x0102030405060708ULL, {0x65, 0xAA}};
        require(
            video_payload::serialize_frame(frame) == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 0x65, 0xAA},
            "Incorrect PTS byte order."
        );

        for (auto pts : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}) {

            frame.pts_us = pts;
            auto decoded_frame = video_payload::parse_frame(video_payload::serialize_frame(frame));
            require(decoded_frame.pts_us == pts && decoded_frame.data == frame.data, "Frame round trip failed.");

        }

        for (bool optimized : {false, true}) {

            auto packets = std::vector<packet>{
                video_payload::make_config(config),
                video_payload::make_frame(frame, true),
                video_payload::make_frame(frame),

                video_payload::make_eos()
            };
            parser receiver;
            receiver.set_optimized(optimized);

            for (auto& pkt : packets) {

                video_payload::validate(pkt);
                pkt.set_flag(ProtocolFlag::OPTS, optimized);
                pkt.set_flag(ProtocolFlag::NACK);
                auto wire = serializer::serialize(pkt);

                for (auto byte : wire)
                    receiver.feed(&byte, 1);
                auto parsed = receiver.next_packet();
                require(parsed.has_value(), "Missing Video packet.");
                video_payload::validate(*parsed);

                require(
                    parsed->payload_flags == pkt.payload_flags && parsed->payload == pkt.payload,
                    "Video packet wire round trip failed."
                );

            }

        }

    }

    void test_invalid_payloads() {

        const auto valid = video_payload::serialize_config({VideoCodec::H264, {0x67}, {0x68}});

        for (std::size_t size = 0; size < valid.size(); size++) {

            rejects([&] {
                (void)video_payload::parse_config(std::span(valid).first(size));
            });

        }

        auto invalid = valid;
        invalid[0] = 0;

        rejects([&] {
            (void)video_payload::parse_config(invalid);
        });

        invalid = valid;
        invalid[1] = 0xFF;

        rejects([&] {
            (void)video_payload::parse_config(invalid);
        });

        invalid = valid;
        invalid.push_back(0);

        rejects([&] {
            (void)video_payload::parse_config(invalid);
        });

        rejects([] {
            (void)video_payload::serialize_config({static_cast<VideoCodec>(255), {}, {}});
        });

        rejects([] {
            (void)video_payload::serialize_config(video_config{});
        });

        rejects([] {
            (void)video_payload::parse_config(std::vector<std::uint8_t>{1, 0, 0, 0, 0, 0, 0, 0, 0});
        });

        auto missing_csd = video_payload::make_eos();
        missing_csd.payload_flags = static_cast<std::uint8_t>(VideoFlag::CONFIG);
        missing_csd.payload = {1, 0, 0, 0, 0, 0, 0, 0, 0};

        rejects([&] {
            video_payload::validate(missing_csd);
        });

        rejects([] {
            (void)video_payload::serialize_config({VideoCodec::H264, {0x67}, {}, 91, false});
        });

        for (const auto tail : {std::vector<std::uint8_t>{0xA1, 4, 0}, {0xA1, 0, 2}, {0xA0, 0, 0}, {0xA1, 0}}) {

            auto malformed = valid;
            malformed.insert(malformed.end(), tail.begin(), tail.end());

            rejects([&] {
                (void)video_payload::parse_config(malformed);
            });

        }

        rejects([] {
            (void)video_payload::parse_config(std::vector<std::uint8_t>{1, 0, 0, 0, 0, 0, 0, 0, 0, 0xA1, 1, 0});
        });

        rejects([] {
            (void)video_payload::serialize_frame({0, {}});
        });

        for (std::size_t size = 0; size <= 8; size++) {

            rejects([&] {
                (void)video_payload::parse_frame(std::vector<std::uint8_t>(size));
            });

        }

        for (int flags = 0; flags < 256; flags++) {

            if (flags == 0 || flags == 1 || flags == 2 || flags == 4 || flags == 8)
                continue;

            auto pkt = video_payload::make_frame({0, {1}});
            pkt.payload_flags = static_cast<std::uint8_t>(flags);

            rejects([&] {
                video_payload::validate(pkt);
            });

        }

        auto eos = video_payload::make_eos();
        eos.payload = {0};

        rejects([&] {
            video_payload::validate(eos);
        });

        eos.payload.clear();
        eos.type = PacketType::Message;

        rejects([&] {
            video_payload::validate(eos);
        });

    }

    void test_video_control_validation() {

        using namespace camdroid::protocol;

        for (const video_format format :
             {video_format{1, 720, 60}, {1281, 720, 60}, {3840, 3840, 60}, {1280, 720, 0}, {1280, 720, 256}})
            rejects([&] {
                (void)make_video_control({1, format});
            });

        auto bitrate_query = make_video_control({7, {1920, 1080, 60}, 0, 6000000});
        require(parse_video_control(bitrate_query).bitrate == 6000000, "Desktop bitrate request was lost.");
        const auto bitrate_reply = make_video_control(parse_video_control(bitrate_query), true);
        require(parse_video_control(bitrate_reply).bitrate == 6000000, "Desktop bitrate response was lost.");
        auto query = make_video_control({1, {1920, 1080, 60}});
        query.payload.push_back(0);

        rejects([&] {
            (void)parse_video_control(query);
        });

        auto response = make_video_control({1, {1920, 1080, 60}, 1}, true);
        response.set_flag(ProtocolFlag::ERR, false);

        rejects([&] {
            (void)parse_video_control(response);
        });

    }

    void test_video_settings() {

        const video_settings settings{60, 8000000};
        auto packet = video_payload::make_settings(settings);
        video_payload::validate(packet);
        require(packet.payload == std::vector<std::uint8_t>{0, 60, 0, 122, 18, 0}, "Incorrect settings wire bytes.");
        const auto owned = video_payload::make_settings({60, 8000000, true});

        require(video_payload::parse_settings(owned.payload).user_configured, "Explicit user format ownership was lost.");
        const auto parsed = video_payload::parse_settings(packet.payload);
        require(parsed.fps == 60 && parsed.bitrate == 8000000, "Settings round trip failed.");
        video_config config{VideoCodec::H264, {0x67}, {0x68}, 90, true, settings};
        const auto decoded = video_payload::parse_config(video_payload::serialize_config(config));

        require(
            decoded.rotation == 90 && decoded.mirrored && decoded.settings.fps == 60 && decoded.settings.bitrate == 8000000,
            "Configuration settings round trip failed."
        );

        for (const auto invalid : {video_settings{0, 1}, {256, 1}, {30, 0}})
            rejects([&] {
                (void)video_payload::make_settings(invalid);
            });

        for (std::size_t size = 0; size < 6; size++)
            rejects([&] {
                (void)video_payload::parse_settings(std::span(packet.payload).first(size));
            });

        packet.payload.push_back(2);

        rejects([&] {
            video_payload::validate(packet);
        });

        packet.payload = {1, 0, 0, 0, 0, 1};

        rejects([&] {
            video_payload::validate(packet);
        });

        auto malformed = video_payload::serialize_config(config);
        malformed[malformed.size() - 7] = 0xA3;

        rejects([&] {
            (void)video_payload::parse_config(malformed);
        });

    }

    void test_integer_helpers() {

        std::vector<std::uint8_t> bytes;
        utils::write_u32(bytes, 0x01020304);
        utils::write_u64(bytes, 0x05060708090A0B0CULL);
        require(
            bytes == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},

            "Incorrect shared integer wire bytes."
        );
        require(
            utils::read_u32(bytes.data()) == 0x01020304 && utils::read_u64(bytes.data() + 4) == 0x05060708090A0B0CULL,

            "Shared fixed-width readers failed."
        );
        std::size_t offset = 0;
        require(utils::read_integer(bytes, offset, 4) == 0x01020304 && offset == 4, "Checked integer reader failed.");

        for (std::size_t width : {std::size_t{0}, std::size_t{9}}) {

            rejects([&] {
                (void)utils::read_integer(bytes, offset, width);
            });

            require(offset == 4, "Failed integer read changed offset.");

            rejects([&] {
                utils::write_integer(bytes, 0, static_cast<int>(width));
            });

        }

        offset = bytes.size() + 1;

        rejects([&] {
            (void)utils::read_integer(bytes, offset, 1);
        });

        offset = bytes.size() - 1;

        rejects([&] {
            (void)utils::read_integer(bytes, offset, 4);
        });

        rejects([&] {
            utils::write_integer(bytes, 0, -1);
        });

    }

};

int main() {

    try {

        using namespace tests;
        test_wire_packets();
        test_invalid_payloads();
        test_integer_helpers();
        test_video_settings();

        test_video_control_validation();
        std::cout << "Video payload checks passed.\n";

        return 0;

    } catch (const std::exception& error) {

        std::cerr << error.what() << '\n';

        return 1;

    }

}