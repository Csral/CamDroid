#ifndef CAMDROID_PROTOCOL_PARSER_H
#define CAMDROID_PROTOCOL_PARSER_H
#pragma once

#include "packet.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace camdroid::protocol {

    class parser {

        private:
        std::vector<std::uint8_t> buffer;
        bool optimized = false;

        std::uint64_t max_payload_size;
        std::uint32_t max_extension_size;

        public:
        explicit parser(std::uint64_t max_payload_size = 320 * 1024 * 1024, std::uint32_t max_extension_size = 1024 * 1024);

        void feed(const void* data, std::size_t size);

        [[nodiscard]]
        std::optional<packet> next_packet();

        void set_optimized(bool enabled);
        void reset();

        [[nodiscard]] bool has_buffered_data() const noexcept {
            return !buffer.empty();
        }

    };

};

#endif