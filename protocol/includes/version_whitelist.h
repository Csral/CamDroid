#ifndef CAMDROID_VERSION_WHITELIST_H
#define CAMDROID_VERSION_WHITELIST_H
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <optional>

namespace camdroid::protocol {

    class version_whitelist {

        private:
        std::array<std::uint64_t, 4> values{};

        public:
        version_whitelist() = default;

        version_whitelist(std::initializer_list<std::uint8_t> initial) {
            for (std::uint8_t value : initial)
                add(value);
        }

        void add(std::uint8_t value) noexcept {
            values[value >> 6] |= 1ULL << (value & 63);
        }

        void remove(std::uint8_t value) noexcept {
            values[value >> 6] &= ~(1ULL << (value & 63));
        }

        [[nodiscard]]
        bool contains(std::uint8_t value) const noexcept {
            return (values[value >> 6] & (1ULL << (value & 63))) != 0;
        }

        [[nodiscard]]
        std::optional<std::uint8_t> find(std::uint8_t jackpot, std::uint8_t low, std::uint8_t high) const noexcept {

            if (low > high)
                return std::nullopt;

            if (jackpot >= low && jackpot <= high && contains(jackpot))
                return jackpot;

            int first = low >> 6;
            int last = high >> 6;

            for (int i = last; i >= first; i--) {

                std::uint64_t mask = values[i];

                if (i == last)
                    mask &= UINT64_MAX >> (63 - (high & 63));

                if (i == first)
                    mask &= UINT64_MAX << (low & 63);

                if (mask != 0)
                    return static_cast<std::uint8_t>(i * 64 + (63 - std::countl_zero(mask)));

            }

            return std::nullopt;

        }

    };

};

#endif