#ifndef CAMDROID_PROTOCOL_SERIALIZER_H
#define CAMDROID_PROTOCOL_SERIALIZER_H
#pragma once

#include "packet.h"

#include <cstdint>
#include <vector>

namespace camdroid::protocol {

    class serializer {

        public:
        [[nodiscard]]
        static std::vector<std::uint8_t> serialize(const packet& pkt);

    };

};

#endif