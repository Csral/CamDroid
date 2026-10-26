#include "utils.h"

#include <stdexcept>

namespace camdroid::protocol {

    namespace utils {

        std::uint64_t read_u64(const std::uint8_t* buffer) {

            std::uint64_t value = 0;

            for (int i = 0; i < 8; i++)
                value = (value << 8) | buffer[i];

            return value;

        }

        std::uint32_t read_u32(const std::uint8_t* buffer) {

            std::uint32_t value = 0;

            for (int i = 0; i < 4; i++)
                value = (value << 8) | buffer[i];

            return value;

        }

        void write_u64(std::vector<std::uint8_t>& buffer, std::uint64_t value) {
            for (int i = 7; i >= 0; i--)
                buffer.push_back(static_cast<std::uint8_t>(value >> (i * 8)));

            return;
        }

        void write_u32(std::vector<std::uint8_t>& buffer, std::uint32_t value) {
            for (int i = 3; i >= 0; i--)
                buffer.push_back(static_cast<std::uint8_t>(value >> (i * 8)));

            return;
        }

        void write_integer(std::vector<std::uint8_t>& buffer, std::uint64_t value, int size) {

            if (size < 1 || size > 8)
                throw std::invalid_argument("Protocol utility failure: Integer width must be 1 to 8 bytes.");

            for (int i = size - 1; i >= 0; i--)
                buffer.push_back(static_cast<std::uint8_t>(value >> (i * 8)));

            return;

        }

        std::uint64_t read_integer(std::span<const std::uint8_t> payload, std::size_t& offset, std::size_t size) {

            if (size < 1 || size > 8 || offset > payload.size() || size > payload.size() - offset)
                throw std::invalid_argument("Protocol utility failure: Invalid integer width or truncated input.");

            std::uint64_t value = 0;

            for (std::size_t i = 0; i < size; i++)
                value = (value << 8) | payload[offset++];

            return value;

        }

    };

}