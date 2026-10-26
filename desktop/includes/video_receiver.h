#pragma once
#ifndef CAMDROID_VIDEO_RECEIVER_H
#define CAMDROID_VIDEO_RECEIVER_H

#include "tcp_socket.h"
#include "video_datagram.h"
#include <chrono>
#include <condition_variable>
#include <map>

#include <mutex>
#include <random>
#include <thread>
#include <exception>
#include <cerrno>

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/select.h>
#endif

namespace camdroid {

    // TCP descriptors order frames with CONFIG/EOS. UDP only carries their bytes.
    class video_receiver {

        private:
        struct assembly {

            std::vector<std::uint8_t> bytes;
            std::vector<bool> received;
            std::size_t count = 0;
            std::chrono::steady_clock::time_point arrived = std::chrono::steady_clock::now();

        };

        #ifdef _WIN32
        SOCKET socket = INVALID_SOCKET;
        #else
        int socket = -1;
        #endif

        protocol::video_datagram::token_type token{};
        std::uint16_t port = 0;
        std::mutex mutex;
        std::condition_variable changed;
        std::map<std::uint64_t, assembly> frames;

        std::size_t buffered = 0;
        std::uint64_t consumed = 0;
        std::exception_ptr failure;
        std::jthread reader;

        void close_socket() noexcept {

            #ifdef _WIN32
            if (socket != INVALID_SOCKET)
                ::closesocket(socket);

            socket = INVALID_SOCKET;
            #else
            if (socket >= 0)
                ::close(socket);

            socket = -1;
            #endif

        }

        void read(std::stop_token stop) noexcept {

            namespace wire = protocol::video_datagram;

            try {

                std::array<std::uint8_t, 1201> bytes{};

                while (!stop.stop_requested()) {

                    fd_set readable;
                    FD_ZERO(&readable);
                    FD_SET(socket, &readable);
                    timeval timeout{0, 100000};
                    const int ready = ::select(static_cast<int>(socket + 1), &readable, nullptr, nullptr, &timeout);

                    if (ready < 0) {

                        #ifndef _WIN32
                        if (errno == EINTR)
                            continue;

                        #endif

                        throw std::runtime_error("UDP video receive failed.");

                    }

                    if (!ready)
                        continue;

                    const auto count = ::recv(socket, reinterpret_cast<char*>(bytes.data()), bytes.size(), 0);

                    if (count < 0) {

                        #ifdef _WIN32
                        const auto error = ::WSAGetLastError();

                        if (error == WSAEMSGSIZE || error == WSAEINTR || error == WSAEWOULDBLOCK)
                            continue;

                        #else
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                            continue;

                        #endif

                        throw std::runtime_error("UDP video socket read failed.");

                    }

                    if (count <= static_cast<int>(wire::header_size) || count > 1200)
                        continue;

                    const auto data = std::span<const std::uint8_t>(bytes.data(), static_cast<std::size_t>(count));

                    if (!std::equal(bytes.begin(), bytes.begin() + 4, "CDU1") ||
                        !std::equal(token.begin(), token.end(), bytes.begin() + 4))
                        continue;

                    const auto id = wire::get(data.subspan(20, 8));
                    const auto offset = static_cast<std::size_t>(wire::get(data.subspan(28, 4)));
                    const auto total = static_cast<std::size_t>(wire::get(data.subspan(32, 4)));
                    const auto length = data.size() - wire::header_size;

                    if (!id || total < 9 || total > wire::max_frame || offset >= total || offset % wire::fragment_size ||
                        length != std::min(wire::fragment_size, total - offset))
                        continue;

                    std::lock_guard lock(mutex);

                    if (id <= consumed || id - consumed > 4096)
                        continue;

                    for (auto entry = frames.begin(); entry != frames.end();) {

                        if (std::chrono::steady_clock::now() - entry->second.arrived > std::chrono::milliseconds(75)) {

                            buffered -= entry->second.bytes.size();
                            entry = frames.erase(entry);

                        } else {

                            ++entry;

                        }

                    }

                    if (!frames.contains(id)) {

                        while (!frames.empty() && (frames.size() >= 32 || buffered + total > 120 * 1024 * 1024)) {

                            if (frames.begin()->first > id)
                                break;

                            buffered -= frames.begin()->second.bytes.size();
                            frames.erase(frames.begin());

                        }

                        if (frames.size() >= 32 || buffered + total > 120 * 1024 * 1024)
                            continue;

                        assembly value;
                        value.bytes.resize(total);
                        value.received.resize((total + wire::fragment_size - 1) / wire::fragment_size);
                        frames.emplace(id, std::move(value));
                        buffered += total;

                    }

                    auto& value = frames.at(id);
                    const auto index = offset / wire::fragment_size;

                    if (value.bytes.size() != total || value.received[index])
                        continue;

                    std::copy(data.begin() + wire::header_size, data.end(), value.bytes.begin() + offset);
                    value.received[index] = true;
                    ++value.count;
                    changed.notify_all();

                }

            } catch (...) {

                std::lock_guard lock(mutex);
                failure = std::current_exception();
                changed.notify_all();

            }

        }

        public:
        video_receiver() {

            try {

                std::random_device random;

                for (auto& byte : token)
                    byte = static_cast<std::uint8_t>(random());

                socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_ANY);

                if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
                    throw std::runtime_error("Cannot bind UDP video receiver.");

                #ifdef _WIN32
                int length = sizeof(address);
                #else
                socklen_t length = sizeof(address);
                #endif

                if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
                    throw std::runtime_error("Cannot obtain UDP video port.");

                port = ntohs(address.sin_port);
                const int capacity = 4 * 1024 * 1024;
                ::setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&capacity), sizeof(capacity));

                reader = std::jthread([this](std::stop_token stop) {
                    read(stop);
                });

            } catch (...) {

                close_socket();
                throw;

            }

        }

        ~video_receiver() {

            reader.request_stop();

            if (reader.joinable())
                reader.join();

            close_socket();

        }

        video_receiver(const video_receiver&) = delete;
        video_receiver& operator=(const video_receiver&) = delete;

        protocol::packet offer() const {

            namespace wire = protocol::video_datagram;
            auto pkt = wire::control(wire::setup, 19);
            wire::put(std::span(pkt.payload).subspan(1, 2), port);
            std::copy(token.begin(), token.end(), pkt.payload.begin() + 3);

            return pkt;

        }

        template <typename Cancelled>
        std::vector<std::uint8_t> take(
            std::uint64_t id,
            std::size_t size,
            bool wait,

            std::chrono::steady_clock::time_point deadline,
            bool keyframe,
            Cancelled cancelled
        ) {

            std::unique_lock lock(mutex);

            const auto complete = [&] {
                const auto it = frames.find(id);

                return it != frames.end() && it->second.count == it->second.received.size();
            };

            while (wait && !complete() && !failure && !cancelled() && std::chrono::steady_clock::now() < deadline) {

                // Prefer recovery over holding newer complete footage behind a missing frame.
                if (!keyframe && std::any_of(frames.upper_bound(id), frames.end(), [](const auto& entry) {
                        return entry.second.count == entry.second.received.size();
                    }))

                    break;

                changed.wait_for(lock, std::chrono::milliseconds(2));

            }

            if (failure)
                std::rethrow_exception(failure);

            std::vector<std::uint8_t> result;
            const auto it = frames.find(id);

            if (complete() && it->second.bytes.size() == size &&
                std::chrono::steady_clock::now() - it->second.arrived <= std::chrono::milliseconds(75))
                result = std::move(it->second.bytes);

            consumed = std::max(consumed, id);

            for (auto entry = frames.begin(); entry != frames.end() && entry->first <= consumed;) {

                entry = frames.erase(entry);

            }

            buffered = 0;

            for (const auto& [number, value] : frames)
                buffered += value.bytes.size();

            return result;

        }

    };

}

#endif