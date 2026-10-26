#include "video_payload.h"
#include "utils.h"

#include <limits>
#include <stdexcept>

namespace camdroid::protocol {

    namespace utils {

        static std::span<const std::uint8_t> read_csd(std::span<const std::uint8_t> payload, std::size_t& offset) {

            std::uint64_t size = read_integer(payload, offset, 4);

            if (size > payload.size() - offset)
                throw std::invalid_argument("Video payload failure: Truncated codec configuration.");

            auto data = payload.subspan(offset, static_cast<std::size_t>(size));
            offset += static_cast<std::size_t>(size);

            return data;

        }

        static void validate_codec(VideoCodec codec) {
            if (codec != VideoCodec::H264)
                throw std::invalid_argument("Video payload failure: Unsupported codec.");

            return;
        }

        static void validate_config(std::span<const std::uint8_t> payload) {

            // Codec (1), two lengths (8), and at least one CSD byte (1).
            if (payload.size() < 10)
                throw std::invalid_argument("Video payload failure: Missing codec configuration.");

            validate_codec(static_cast<VideoCodec>(payload[0]));
            std::size_t offset = 1;
            const auto csd0 = read_csd(payload, offset);
            const auto csd1 = read_csd(payload, offset);

            if (csd0.empty() && csd1.empty())
                throw std::invalid_argument("Video payload failure: Missing codec configuration.");

            if (offset != payload.size()) {

                // Optional display metadata: tag, quarter turns, horizontal mirror.
                if ((payload.size() - offset != 3 && payload.size() - offset != 10 && payload.size() - offset != 11) ||
                    payload[offset] != 0xA1 || payload[offset + 1] > 3 || payload[offset + 2] > 1)
                    throw std::invalid_argument("Video payload failure: Invalid display metadata.");

                if (payload.size() - offset >= 10) {

                    if (payload[offset + 3] != 0xA2)
                        throw std::invalid_argument("Invalid video settings metadata.");

                    video_payload::parse_settings(payload.subspan(offset + 4));

                }

            }

            return;

        }

    };

    std::vector<std::uint8_t> video_payload::serialize_config(const video_config& config) {

        utils::validate_codec(config.codec);

        if (config.rotation > 270 || config.rotation % 90 != 0)
            throw std::invalid_argument("Video payload failure: Rotation must be a quarter turn.");

        if (config.csd0.empty() && config.csd1.empty())
            throw std::invalid_argument("Video payload failure: Missing codec configuration.");

        if (config.csd0.size() > std::numeric_limits<std::uint32_t>::max() ||
            config.csd1.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("Video payload failure: Codec configuration exceeds uint32 size.");

        std::vector<std::uint8_t> buffer;

        if (buffer.max_size() < 9 || config.csd0.size() > buffer.max_size() - 9 ||
            config.csd1.size() > buffer.max_size() - 9 - config.csd0.size())
            throw std::length_error("Video payload failure: Codec configuration size overflow.");

        buffer.reserve(9 + config.csd0.size() + config.csd1.size());
        buffer.push_back(static_cast<std::uint8_t>(config.codec));
        utils::write_integer(buffer, config.csd0.size(), 4);
        buffer.insert(buffer.end(), config.csd0.begin(), config.csd0.end());
        utils::write_integer(buffer, config.csd1.size(), 4);
        buffer.insert(buffer.end(), config.csd1.begin(), config.csd1.end());

        if (config.rotation != 0 || config.mirrored || config.settings.fps || config.settings.bitrate) {

            buffer.push_back(0xA1);
            buffer.push_back(static_cast<std::uint8_t>(config.rotation / 90));
            buffer.push_back(config.mirrored ? 1 : 0);

        }

        if (config.settings.fps || config.settings.bitrate) {

            const auto settings = make_settings(config.settings);
            buffer.push_back(0xA2);
            buffer.insert(buffer.end(), settings.payload.begin(), settings.payload.end());

        }

        return buffer;

    }

    video_config video_payload::parse_config(std::span<const std::uint8_t> payload) {

        utils::validate_config(payload);

        video_config config;
        config.codec = static_cast<VideoCodec>(payload[0]);
        std::size_t offset = 1;
        auto csd0 = utils::read_csd(payload, offset);
        auto csd1 = utils::read_csd(payload, offset);

        config.csd0.assign(csd0.begin(), csd0.end());
        config.csd1.assign(csd1.begin(), csd1.end());

        if (offset != payload.size()) {

            config.rotation = payload[offset + 1] * 90;
            config.mirrored = payload[offset + 2] != 0;

            if (payload.size() - offset >= 10)
                config.settings = parse_settings(payload.subspan(offset + 4));

        }

        return config;

    }

    std::vector<std::uint8_t> video_payload::serialize_frame(const video_frame& frame) {

        if (frame.data.empty())
            throw std::invalid_argument("Video payload failure: Empty encoded frame.");

        std::vector<std::uint8_t> buffer;

        if (buffer.max_size() < 8 || frame.data.size() > buffer.max_size() - 8)
            throw std::length_error("Video payload failure: Frame size overflow.");

        buffer.reserve(8 + frame.data.size());
        utils::write_integer(buffer, frame.pts_us, 8);
        buffer.insert(buffer.end(), frame.data.begin(), frame.data.end());

        return buffer;

    }

    video_frame video_payload::parse_frame(std::span<const std::uint8_t> payload) {

        if (payload.size() <= 8)
            throw std::invalid_argument("Video payload failure: Missing timestamp or encoded frame.");

        video_frame frame;
        std::size_t offset = 0;
        frame.pts_us = utils::read_integer(payload, offset, 8);
        frame.data.assign(payload.begin() + offset, payload.end());

        return frame;

    }

    packet video_payload::make_config(const video_config& config) {

        packet pkt;
        pkt.type = PacketType::Video;
        pkt.payload_flags = static_cast<std::uint8_t>(VideoFlag::CONFIG);
        pkt.payload = serialize_config(config);

        return pkt;

    }

    packet video_payload::make_frame(const video_frame& frame, bool keyframe) {

        packet pkt;
        pkt.type = PacketType::Video;
        pkt.payload_flags = keyframe ? static_cast<std::uint8_t>(VideoFlag::KEYFRAME) : 0;
        pkt.payload = serialize_frame(frame);

        return pkt;

    }

    packet video_payload::make_eos() {

        packet pkt;
        pkt.type = PacketType::Video;
        pkt.payload_flags = static_cast<std::uint8_t>(VideoFlag::EOS);

        return pkt;

    }

    packet video_payload::make_settings(video_settings settings) {

        if (settings.fps == 0 || settings.fps > 255 || settings.bitrate == 0)
            throw std::invalid_argument("Invalid configured video FPS or bitrate.");

        packet pkt;
        pkt.type = PacketType::Video;
        pkt.payload_flags = static_cast<std::uint8_t>(VideoFlag::SETTINGS);
        utils::write_integer(pkt.payload, settings.fps, 2);
        utils::write_integer(pkt.payload, settings.bitrate, 4);

        if (settings.user_configured)
            pkt.payload.push_back(1);

        return pkt;

    }

    video_settings video_payload::parse_settings(std::span<const std::uint8_t> payload) {

        if (payload.size() != 6 && payload.size() != 7)
            throw std::invalid_argument("Invalid video settings size.");

        std::size_t offset = 0;
        video_settings settings;
        settings.fps = static_cast<std::uint16_t>(utils::read_integer(payload, offset, 2));
        settings.bitrate = static_cast<std::uint32_t>(utils::read_integer(payload, offset, 4));

        if (payload.size() == 7) {

            if (payload[6] > 1)
                throw std::invalid_argument("Invalid video settings ownership marker.");

            settings.user_configured = payload[6] != 0;

        }

        if (!settings.fps || settings.fps > 255 || !settings.bitrate)
            throw std::invalid_argument("Invalid configured video FPS or bitrate.");

        return settings;

    }

    void video_payload::validate(const packet& pkt) {

        if (pkt.type != PacketType::Video)
            throw std::invalid_argument("Video payload failure: Expected Video packet.");

        switch (pkt.payload_flags) {

            case static_cast<std::uint8_t>(VideoFlag::SETTINGS):
                (void)parse_settings(pkt.payload);
                break;
            case static_cast<std::uint8_t>(VideoFlag::CONFIG):
                utils::validate_config(pkt.payload);
                break;

            case 0:
            case static_cast<std::uint8_t>(VideoFlag::KEYFRAME):
                if (pkt.payload.size() <= 8)
                    throw std::invalid_argument("Video payload failure: Missing timestamp or encoded frame.");

                break;

            case static_cast<std::uint8_t>(VideoFlag::EOS):
                if (!pkt.payload.empty())
                    throw std::invalid_argument("Video payload failure: EOS payload must be empty.");

                break;

            default:
                throw std::invalid_argument("Video payload failure: Invalid payload flags.");

        }

        return;

    }

}