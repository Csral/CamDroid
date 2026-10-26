#ifndef CAMDROID_PROTOCOL_VIDEO_PAYLOAD_H
#define CAMDROID_PROTOCOL_VIDEO_PAYLOAD_H
#pragma once

#include "packet.h"

#include <cstdint>
#include <span>
#include <vector>

namespace camdroid::protocol {

    enum class VideoFlag : std::uint8_t {

        CONFIG = 1u << 0,
        KEYFRAME = 1u << 1,
        EOS = 1u << 2,
        SETTINGS = 1u << 3

    };

    enum class VideoCodec : std::uint8_t {

        // 0x00 is reserved until a raw video format is defined.
        H264 = 0x01

    };

    struct video_settings {

        std::uint16_t fps = 0;
        std::uint32_t bitrate = 0;
        bool user_configured = false;

    };

    struct video_config {

        VideoCodec codec = VideoCodec::H264;
        std::vector<std::uint8_t> csd0;
        std::vector<std::uint8_t> csd1;
        // Clockwise display rotation; mirror horizontally after rotation.
        std::uint16_t rotation = 0;

        bool mirrored = false;
        video_settings settings;

    };

    struct video_frame {

        // Presentation time in microseconds since the first frame after CONFIG.
        std::uint64_t pts_us = 0;
        // One complete encoded access unit, not a partial encoder buffer.
        std::vector<std::uint8_t> data;

    };

    class video_payload {

        public:
        [[nodiscard]]
        static std::vector<std::uint8_t> serialize_config(const video_config& config);

        [[nodiscard]]
        static video_config parse_config(std::span<const std::uint8_t> payload);

        [[nodiscard]]
        static std::vector<std::uint8_t> serialize_frame(const video_frame& frame);

        [[nodiscard]]
        static video_frame parse_frame(std::span<const std::uint8_t> payload);

        // Helpers set only the Video type, payload flags and payload. The caller
        // supplies protocol flags, version and Unix timestamp before sending.
        [[nodiscard]]
        static packet make_config(const video_config& config);

        [[nodiscard]]
        static packet make_frame(const video_frame& frame, bool keyframe = false);

        [[nodiscard]]
        static packet make_eos();
        static packet make_settings(video_settings settings);
        static video_settings parse_settings(std::span<const std::uint8_t> payload);

        // Checks packet contents only. CONFIG/keyframe ordering and PTS timeline
        // enforcement belong to the stream owner, not this stateless helper.
        static void validate(const packet& pkt);

    };

};

#endif