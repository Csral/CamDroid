#ifndef CAMDROID_TCP_SOCKET_H
#define CAMDROID_TCP_SOCKET_H
#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>

#include <netinet/in.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace camdroid {

    namespace utils {

        std::vector<std::string> local_ipv4_addresses();
        std::string local_device_name();

    };

    class tcp_socket {

        private:
        #ifdef _WIN32
        SOCKET socket_handler = INVALID_SOCKET;
        #else
        int socket_handler = -1;
        #endif

        #ifdef _WIN32
        bool socket_wsa_startup_finished = false;
        #endif

        bool negotiate_port = false;
        std::uint8_t negotiation_attempts = -1;

        #ifdef _WIN32
        explicit tcp_socket(SOCKET socket_handler);
        #else
        explicit tcp_socket(int socket_handler);
        #endif

        public:
        tcp_socket(tcp_socket&& other) noexcept;
        tcp_socket();
        tcp_socket(bool negotiation_port, std::uint8_t num_negotiation_attempts);
        ~tcp_socket();

        std::uint16_t bind(std::uint16_t port);
        void set_nonblocking(bool enabled);
        void set_send_timeout(int timeout_ms);
        void set_low_latency();
        std::optional<tcp_socket> try_accept();
        void listen(void);

        tcp_socket accept();
        std::size_t recv(void* buffer, std::size_t size);
        void send_all(const void* data, std::size_t size);

        bool wait_readable(int timeout_ms);
        void interrupt() noexcept;

        tcp_socket(const tcp_socket&) = delete;
        tcp_socket& operator=(tcp_socket& other) = delete;
        tcp_socket& operator=(tcp_socket&& other) noexcept;

    };

};

#endif