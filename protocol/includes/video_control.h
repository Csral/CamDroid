#pragma once
#ifndef CAMDROID_VIDEO_CONTROL_H
#define CAMDROID_VIDEO_CONTROL_H
#include "packet.h"
#include "utils.h"
#include <span>
#include <vector>
#include <utility>

#include <algorithm>
#include <stdexcept>

namespace camdroid::protocol {

    struct video_format {

        std::uint16_t width = 1280;
        std::uint16_t height = 720;
        std::uint16_t fps = 30;
        bool operator==(const video_format&) const = default;

    };

    inline constexpr std::uint32_t adaptive_bitrate = 0xffffffff;

    struct video_control {

        std::uint32_t id = 0;
        video_format format;
        std::uint8_t status = 0;
        std::uint32_t bitrate = 0; // Zero preserves the current mode adaptive_bitrate explicitly selects adaptive.

    };

    inline video_control parse_video_control(const packet& pkt) {

        const bool response = pkt.has_flag(ProtocolFlag::ACK);
        const std::size_t base = response ? 12 : 11;

        if (pkt.type != PacketType::Communication || (pkt.payload.size() != base && pkt.payload.size() != base + 4) ||
            pkt.payload[0] != 0x10 || pkt.payload_flags || pkt.has_flag(ProtocolFlag::QRY) == response ||
            pkt.has_flag(ProtocolFlag::FIN) || pkt.has_flag(ProtocolFlag::NACK))
            throw std::invalid_argument("Invalid video format negotiation.");

        std::size_t offset = 1;
        video_control result;
        result.id = static_cast<std::uint32_t>(utils::read_integer(pkt.payload, offset, 4));
        result.format.width = static_cast<std::uint16_t>(utils::read_integer(pkt.payload, offset, 2));
        result.format.height = static_cast<std::uint16_t>(utils::read_integer(pkt.payload, offset, 2));
        result.format.fps = static_cast<std::uint16_t>(utils::read_integer(pkt.payload, offset, 2));

        if (pkt.payload.size() == base + 4)
            result.bitrate = static_cast<std::uint32_t>(utils::read_integer(pkt.payload, offset, 4));

        if (response)
            result.status = pkt.payload.back();

        if (!result.id || result.format.width < 2 || result.format.height < 2 || result.format.width > 3840 ||
            result.format.height > 3840 || result.format.width % 2 || result.format.height % 2 ||
            static_cast<std::uint64_t>(result.format.width) * result.format.height > 3840ULL * 2160 || !result.format.fps ||
            result.format.fps > 255 || result.status > 3 ||
            (result.bitrate > 0x7fffffff && result.bitrate != adaptive_bitrate) ||
            pkt.has_flag(ProtocolFlag::ERR) != (response && result.status != 0))

            throw std::invalid_argument("Invalid requested video format.");

        return result;

    }

    inline packet make_video_control(video_control control, bool response = false) {

        packet pkt;
        pkt.type = PacketType::Communication;
        pkt.set_flag(response ? ProtocolFlag::ACK : ProtocolFlag::QRY);

        if (response && control.status)
            pkt.set_flag(ProtocolFlag::ERR);

        pkt.payload.push_back(0x10);
        utils::write_integer(pkt.payload, control.id, 4);
        utils::write_integer(pkt.payload, control.format.width, 2);
        utils::write_integer(pkt.payload, control.format.height, 2);
        utils::write_integer(pkt.payload, control.format.fps, 2);

        if (control.bitrate)
            utils::write_integer(pkt.payload, control.bitrate, 4);

        if (response)
            pkt.payload.push_back(control.status);

        (void)parse_video_control(pkt);

        return pkt;

    }

    inline std::uint32_t parse_camera_flip(const packet& pkt) {

        const bool response = pkt.has_flag(ProtocolFlag::ACK);
        const auto allowed = static_cast<std::uint8_t>(ProtocolFlag::OPTS) |
                             static_cast<std::uint8_t>(response ? ProtocolFlag::ACK : ProtocolFlag::QRY) |
                             (response ? static_cast<std::uint8_t>(ProtocolFlag::ERR) : 0);

        if (pkt.type != PacketType::Communication || pkt.payload_flags || !pkt.extensions.empty() ||
            pkt.payload.size() != (response ? 6 : 5) || pkt.payload[0] != 0x11 ||
            !pkt.has_flag(response ? ProtocolFlag::ACK : ProtocolFlag::QRY) || (pkt.flags & ~allowed))
            throw std::invalid_argument("Invalid camera flip request.");

        std::size_t offset = 1;
        const auto id = static_cast<std::uint32_t>(utils::read_integer(pkt.payload, offset, 4));

        if (!id || (response && (pkt.payload.back() > 3 || pkt.has_flag(ProtocolFlag::ERR) != (pkt.payload.back() != 0))))
            throw std::invalid_argument("Invalid camera flip response.");

        return id;

    }

    inline packet make_camera_flip(std::uint32_t id) {

        packet pkt;
        pkt.type = PacketType::Communication;
        pkt.set_flag(ProtocolFlag::QRY);
        pkt.payload.push_back(0x11);
        utils::write_integer(pkt.payload, id, 4);
        (void)parse_camera_flip(pkt);

        return pkt;

    }

    struct video_feedback {

        std::uint32_t expected = 0;
        std::uint32_t usable = 0;
        std::uint16_t queue_ms = 0;

    };

    inline video_feedback parse_video_feedback(const packet& pkt) {

        const auto allowed = static_cast<std::uint8_t>(ProtocolFlag::OPTS);

        if (pkt.type != PacketType::Communication || pkt.payload_flags || !pkt.extensions.empty() ||
            pkt.payload.size() != 11 || pkt.payload[0] != 0x12 || (pkt.flags & ~allowed))
            throw std::invalid_argument("Invalid video delivery feedback.");

        std::size_t offset = 1;
        video_feedback result;
        result.expected = static_cast<std::uint32_t>(utils::read_integer(pkt.payload, offset, 4));
        result.usable = static_cast<std::uint32_t>(utils::read_integer(pkt.payload, offset, 4));
        result.queue_ms = static_cast<std::uint16_t>(utils::read_integer(pkt.payload, offset, 2));

        if (result.usable > result.expected)
            throw std::invalid_argument("Invalid video delivery counts.");

        return result;

    }

    inline packet make_video_feedback(video_feedback feedback) {

        packet pkt;
        pkt.type = PacketType::Communication;
        pkt.payload.push_back(0x12);
        utils::write_integer(pkt.payload, feedback.expected, 4);
        utils::write_integer(pkt.payload, feedback.usable, 4);
        utils::write_integer(pkt.payload, feedback.queue_ms, 2);

        (void)parse_video_feedback(pkt);
        return pkt;

    }

    struct resolution_capabilities {

        video_format format;
        std::uint32_t min_bitrate = 0;
        std::uint32_t max_bitrate = 0;
        std::vector<std::uint16_t> rates;

    };

    struct camera_capabilities {

        std::uint16_t width = 0, height = 0;
        bool front = false;
        std::vector<resolution_capabilities> resolutions;

    };

    struct capture_selection {

        std::uint16_t fps = 0;     // Zero selects the Android default.
        std::uint32_t bitrate = 0; // Zero selects adaptive bitrate.

    };

    inline void validate_capture_announcement(const packet& pkt) {
        const auto allowed = static_cast<std::uint8_t>(ProtocolFlag::OPTS);

        if (pkt.type != PacketType::Communication || pkt.payload_flags || !pkt.extensions.empty() || (pkt.flags & ~allowed))
            throw std::invalid_argument("Invalid capture announcement flags.");
    }

    inline camera_capabilities parse_camera_capabilities(const packet& pkt) {

        validate_capture_announcement(pkt);
        if (pkt.payload.size() < 7 || pkt.payload[0] != 0x13 || pkt.payload[1] > 1 || pkt.payload[6] > 8)
            throw std::invalid_argument("Invalid camera capabilities.");

        camera_capabilities result;
        result.front = pkt.payload[1] != 0;
        std::size_t offset = 2;
        result.width = static_cast<std::uint16_t>(utils::read_integer(pkt.payload, offset, 2));
        result.height = static_cast<std::uint16_t>(utils::read_integer(pkt.payload, offset, 2));
        const auto count = utils::read_integer(pkt.payload, offset, 1);

        const auto valid_size = [](std::uint16_t w, std::uint16_t h) {
            return w >= 2 && h >= 2 && w <= 3840 && h <= 3840 && !(w % 2) && !(h % 2) &&
                   static_cast<std::uint64_t>(w) * h <= 3840ULL * 2160;
        };

        if (!valid_size(result.width, result.height))
            throw std::invalid_argument("Invalid capability resolution.");

        for (std::size_t i = 0; i < count; ++i) {

            resolution_capabilities entry;
            entry.format.width = static_cast<std::uint16_t>(utils::read_integer(pkt.payload, offset, 2));
            entry.format.height = static_cast<std::uint16_t>(utils::read_integer(pkt.payload, offset, 2));
            entry.min_bitrate = static_cast<std::uint32_t>(utils::read_integer(pkt.payload, offset, 4));
            entry.max_bitrate = static_cast<std::uint32_t>(utils::read_integer(pkt.payload, offset, 4));
            const auto rates = utils::read_integer(pkt.payload, offset, 1);

            if (!valid_size(entry.format.width, entry.format.height) || !entry.min_bitrate ||
                entry.max_bitrate < entry.min_bitrate || entry.max_bitrate > 0x7fffffff || !rates || rates > 240 ||
                std::any_of(result.resolutions.begin(), result.resolutions.end(), [&](const auto& previous) {
                    return previous.format.width == entry.format.width && previous.format.height == entry.format.height;
                }))

                throw std::invalid_argument("Invalid resolution capabilities.");

            for (std::size_t j = 0; j < rates; ++j) {

                const auto rate = static_cast<std::uint16_t>(utils::read_integer(pkt.payload, offset, 2));
                if (!rate || rate > 240 || (!entry.rates.empty() && rate <= entry.rates.back()))
                    throw std::invalid_argument("Invalid supported frame rate.");

                entry.rates.push_back(rate);

            }

            result.resolutions.push_back(std::move(entry));

        }

        if (offset != pkt.payload.size())
            throw std::invalid_argument("Trailing camera capability bytes.");

        return result;

    }

    inline capture_selection parse_capture_selection(const packet& pkt) {

        validate_capture_announcement(pkt);
        if (pkt.payload.size() != 7 || pkt.payload[0] != 0x14)
            throw std::invalid_argument("Invalid capture selection.");

        std::size_t offset = 1;
        capture_selection result;
        result.fps = static_cast<std::uint16_t>(utils::read_integer(pkt.payload, offset, 2));
        result.bitrate = static_cast<std::uint32_t>(utils::read_integer(pkt.payload, offset, 4));

        if (result.fps > 240 || result.bitrate > 0x7fffffff)
            throw std::invalid_argument("Invalid capture selection values.");

        return result;

    }

    struct video_orientation {

        std::uint64_t first_frame = 0;
        std::uint16_t rotation = 0;

    };

    inline video_orientation parse_video_orientation(const packet& pkt) {

        validate_capture_announcement(pkt);
        if (pkt.payload.size() != 11 || pkt.payload[0] != 0x15)
            throw std::invalid_argument("Invalid video orientation update.");

        std::size_t offset = 1;
        video_orientation result;
        result.first_frame = utils::read_integer(pkt.payload, offset, 8);
        result.rotation = static_cast<std::uint16_t>(utils::read_integer(pkt.payload, offset, 2));

        if (!result.first_frame || result.rotation > 270 || result.rotation % 90)
            throw std::invalid_argument("Invalid video orientation values.");

        return result;

    }

    inline packet make_video_orientation(video_orientation orientation) {

        packet pkt;
        pkt.type = PacketType::Communication;
        pkt.payload.push_back(0x15);
        utils::write_integer(pkt.payload, orientation.first_frame, 8);
        utils::write_integer(pkt.payload, orientation.rotation, 2);
        (void)parse_video_orientation(pkt);

        return pkt;

    }

};

#endif