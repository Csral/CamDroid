#ifndef CAMDROID_PROTOCOL_H
#define CAMDROID_PROTOCOL_H
#pragma once

#include <cstddef>
#include <cstdint>
#include "version_whitelist.h"

namespace camdroid::protocol {

    inline constexpr char MAGIC[3] = {'N', 'y', 'a'};
    inline constexpr unsigned char version = 1;
    inline constexpr unsigned char min_supported_version = 1;
    inline constexpr unsigned char max_supported_version = 1;
    inline const version_whitelist supported_versions{1};

    enum class PacketType : std::uint8_t {

        Config = 0x0,
        Communication = 0x1,
        Message = 0x2,
        VideoConfig = 0x3,
        Video = 0x4

    };

    enum class ProtocolFlag : std::uint8_t {

        ACK = 1u << 0,
        ERR = 1u << 1,
        QRY = 1u << 2,
        OPTS = 1u << 3,
        NACK = 1u << 4,
        FIN = 1u << 5,

    };

    inline constexpr std::size_t NORMAL_HEADER_SIZE = 28;
    inline constexpr std::size_t OPTS_HEADER_SIZE = 20;

}

#endif