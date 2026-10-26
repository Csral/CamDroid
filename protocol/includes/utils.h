#ifndef CAMDROID_PROTOCOL_UTILS_H
#define CAMDROID_PROTOCOL_UTILS_H
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace camdroid::protocol {

    namespace utils {

        // All integer helpers use big-endian wire order.
        // Pointer overloads require at least 4 or 8 readable bytes respectively.
        [[nodiscard]]
        std::uint32_t read_u32(const std::uint8_t* buffer);

        [[nodiscard]]
        std::uint64_t read_u64(const std::uint8_t* buffer);

        void write_u32(std::vector<std::uint8_t>& buffer, std::uint32_t value);
        void write_u64(std::vector<std::uint8_t>& buffer, std::uint64_t value);

        // Checked reader: size must be 1 to 8 bytes. Advances offset only on success.
        [[nodiscard]]
        std::uint64_t read_integer(std::span<const std::uint8_t> payload, std::size_t& offset, std::size_t size);

        // Appends size bytes (1 to 8), retaining the low-order bytes of value.
        void write_integer(std::vector<std::uint8_t>& buffer, std::uint64_t value, int size);

    };

};

#endif