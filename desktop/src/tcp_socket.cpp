#include "tcp_socket.h"
#include <stdexcept>
#include <string>
#include <climits>
#include <algorithm>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>

#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

#ifndef _WIN32
#include <sys/select.h>
#endif

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#endif

namespace camdroid {

    namespace utils {

        std::string local_device_name() {

            char name[256]{};

            if (::gethostname(name, sizeof(name) - 1) != 0 || name[0] == 0)
                return "CamDroid desktop";

            return std::string(name).substr(0, 63);

        }

        std::vector<std::string> local_ipv4_addresses() {

            std::vector<std::string> addresses;

            const auto append = [&](const sockaddr* address) {

                if (!address || address->sa_family != AF_INET)
                    return;

                const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
                const auto value = ntohl(ipv4->sin_addr.s_addr);

                if (value == 0 || (value >> 24) == 127 || (value >> 16) == 0xA9FE)
                    return;

                char text[INET_ADDRSTRLEN]{};

                if (::inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)))
                    addresses.emplace_back(text);

            };

            #ifdef _WIN32
            ULONG size = 16384;
            std::vector<unsigned char> storage(size);
            auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
            ULONG result = ::GetAdaptersAddresses(
                AF_INET,

                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr,
                adapters,
                &size
            );

            if (result == ERROR_BUFFER_OVERFLOW) {

                storage.resize(size);
                adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
                result = ::GetAdaptersAddresses(
                    AF_INET,
                    GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,

                    nullptr,
                    adapters,
                    &size
                );

            }

            if (result == NO_ERROR) {

                for (auto* adapter = adapters; adapter; adapter = adapter->Next) {

                    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                        continue;

                    for (auto* entry = adapter->FirstUnicastAddress; entry; entry = entry->Next)
                        append(entry->Address.lpSockaddr);

                }

            }

            #else
            ifaddrs* interfaces = nullptr;

            if (::getifaddrs(&interfaces) == 0) {

                for (auto* entry = interfaces; entry; entry = entry->ifa_next) {

                    if ((entry->ifa_flags & IFF_UP) && !(entry->ifa_flags & IFF_LOOPBACK))
                        append(entry->ifa_addr);

                }

                ::freeifaddrs(interfaces);

            }

            #endif
            std::sort(addresses.begin(), addresses.end());
            addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());

            if (addresses.empty())
                addresses.emplace_back("127.0.0.1");

            return addresses;

        }

    };

    tcp_socket& tcp_socket::operator=(tcp_socket&& other) noexcept {

        if (this == &other)
            return *this;

        #ifdef _WIN32
        if (this->socket_handler != INVALID_SOCKET) {

            closesocket(socket_handler); // ignore any errors.

        }

        if (this->socket_wsa_startup_finished) {

            WSACleanup();
            this->socket_wsa_startup_finished = false;

        }

        #else
        if (this->socket_handler > -1) {

            close(this->socket_handler);

        }

        #endif

        this->socket_handler = other.socket_handler;

        #ifdef _WIN32
        this->socket_wsa_startup_finished = other.socket_wsa_startup_finished;
        other.socket_wsa_startup_finished = false;
        #endif

        this->negotiate_port = other.negotiate_port;
        this->negotiation_attempts = other.negotiation_attempts;

        #ifdef _WIN32
        other.socket_handler = INVALID_SOCKET;
        #else
        other.socket_handler = -1;
        #endif

        return *this;

    }

    tcp_socket::tcp_socket(tcp_socket&& other) noexcept {

        this->socket_handler = other.socket_handler;

        #ifdef _WIN32
        this->socket_wsa_startup_finished = other.socket_wsa_startup_finished;

        other.socket_wsa_startup_finished = false;
        #endif

        this->negotiate_port = other.negotiate_port;
        this->negotiation_attempts = other.negotiation_attempts;

        #ifdef _WIN32
        other.socket_handler = INVALID_SOCKET;
        #else
        other.socket_handler = -1;
        #endif

        return;

    }

    tcp_socket::tcp_socket() {

        #ifdef _WIN32

        WSADATA wsa_data;

        int res = WSAStartup(MAKEWORD(2, 2), &wsa_data);

        if (res != 0)
            throw std::runtime_error("Socket creation failed.");

        socket_wsa_startup_finished = true;

        socket_handler = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (socket_handler == INVALID_SOCKET) {

            WSACleanup();
            socket_wsa_startup_finished = false;
            throw std::runtime_error("Socket handler creation failed.");

        }

        #else

        socket_handler = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (socket_handler < 0)
            throw std::runtime_error("Socket handler creation failed.");

        #endif

    }

    tcp_socket::tcp_socket(bool negotiation_port, std::uint8_t attempts) : tcp_socket() {
        this->negotiate_port = negotiation_port;
        this->negotiation_attempts = attempts;
    }

    tcp_socket::~tcp_socket() {

        #ifdef _WIN32

        if (socket_handler != INVALID_SOCKET) {

            closesocket(socket_handler);
            socket_handler = INVALID_SOCKET;

        }

        if (socket_wsa_startup_finished) {

            WSACleanup();
            socket_wsa_startup_finished = false;

        }

        #else

        if (socket_handler > -1) {

            close(socket_handler);
            socket_handler = -1;

        }

        #endif

    }

    #ifdef _WIN32
    tcp_socket::tcp_socket(SOCKET socket_handler) : socket_handler(socket_handler) {

        WSADATA wsa_data;

        int res = WSAStartup(MAKEWORD(2, 2), &wsa_data);

        if (res != 0) {

            closesocket(socket_handler);
            throw std::runtime_error("Socket creation failed.");

        }

        this->socket_wsa_startup_finished = true;

    };

    #else
    tcp_socket::tcp_socket(int socket_handler) : socket_handler(socket_handler) {
        return;
    }

    #endif

    std::uint16_t tcp_socket::bind(std::uint16_t port) {

        if (port < 4097)
            throw std::invalid_argument("Port must be between 4097 and 65535.");

        #ifdef _WIN32
        const BOOL exclusive = TRUE;

        if (::setsockopt(
                socket_handler,
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive),

                sizeof(exclusive)
            ) == SOCKET_ERROR)
            throw std::runtime_error("Cannot configure exclusive listening socket.");

        #endif

        const unsigned int attempts = negotiate_port ? negotiation_attempts : 0;

        for (unsigned int attempt = 0; attempt <= attempts; attempt++) {

            const auto candidate = static_cast<std::uint16_t>(4097 + (port - 4097 + attempt) % (65536 - 4097));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(candidate);
            address.sin_addr.s_addr = htonl(INADDR_ANY);

            if (::bind(socket_handler, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
                return candidate;

            #ifdef _WIN32
            const int error = WSAGetLastError();

            if (error != WSAEADDRINUSE)
                throw std::runtime_error("Socket binding failure: " + std::to_string(error));

            #else
            const int error = errno;

            if (error != EADDRINUSE)
                throw std::runtime_error("Socket binding failure: " + std::to_string(error));

            #endif

        }

        throw std::runtime_error("Port is in use; all five alternative ports are unavailable.");

    }

    void tcp_socket::set_nonblocking(bool enabled) {

        #ifdef _WIN32
        u_long mode = enabled ? 1 : 0;

        if (::ioctlsocket(socket_handler, FIONBIO, &mode) != 0)
            throw std::runtime_error("Cannot change socket blocking mode.");

        #else
        const int flags = ::fcntl(socket_handler, F_GETFL, 0);

        if (flags < 0 || ::fcntl(socket_handler, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) < 0)
            throw std::runtime_error("Cannot change socket blocking mode.");

        #endif

    }

    void tcp_socket::set_send_timeout(int timeout_ms) {

        #ifdef _WIN32
        const DWORD timeout = static_cast<DWORD>(timeout_ms);
        const int result =
            ::setsockopt(socket_handler, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        #else
        const timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        const int result = ::setsockopt(socket_handler, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        #endif
        if (result != 0)
            throw std::runtime_error("Cannot configure socket send timeout.");

    }

    void tcp_socket::set_low_latency() {

        const int enabled = 1;

        if (::setsockopt(
                socket_handler,
                IPPROTO_TCP,
                TCP_NODELAY,
                reinterpret_cast<const char*>(&enabled),

                sizeof(enabled)
            ) != 0)
            throw std::runtime_error("Cannot configure low latency TCP controls.");

    }

    std::optional<tcp_socket> tcp_socket::try_accept() {

        const auto handle = ::accept(socket_handler, nullptr, nullptr);
        #ifdef _WIN32
        if (handle == INVALID_SOCKET) {

            const int error = WSAGetLastError();

            if (error == WSAEWOULDBLOCK || error == WSAEINTR || error == WSAECONNRESET)
                return std::nullopt;

            throw std::runtime_error("Socket accept failure: " + std::to_string(error));

        }

        #else
        if (handle < 0) {

            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED)
                return std::nullopt;

            throw std::runtime_error("Socket accept failure: " + std::to_string(errno));

        }

        #endif
        tcp_socket peer(handle);
        peer.set_nonblocking(false);

        return peer;

    }

    void tcp_socket::listen(void) {

        int res = ::listen(socket_handler, SOMAXCONN);

        #ifdef _WIN32

        if (res == SOCKET_ERROR) {

            switch (WSAGetLastError()) {

                case WSANOTINITIALISED:
                    throw std::runtime_error("Socket listening failure: Socket not initialized.");
                    break;

                case WSAENETDOWN:
                    throw std::runtime_error("Socket listening failure: Network module down.");
                    break;

                case WSAEADDRINUSE:
                    throw std::runtime_error("Socket listening failure: Port in use.");
                    break;

                case WSAEINPROGRESS:
                    throw std::runtime_error("Socket listening failure: A binding in progress.");
                    break;

                case WSAEINVAL:
                    throw std::runtime_error("Socket listening failure: Socket not bound.");
                    break;

                case WSAEISCONN:
                    throw std::runtime_error("Socket listening failure: Socket already connected.");
                    break;

                default:
                    throw std::runtime_error("Socket listening failure.");
                    break;

            }

        }

        #else

        if (res == -1) {

            switch (errno) {

                case EADDRINUSE:
                    throw std::runtime_error("Socket listening failure: Port in use.");
                    break;

                case EOPNOTSUPP:
                    throw std::runtime_error("Socket doesn't support listening.");
                    break;

                default:
                    throw std::runtime_error("Socket listening failure.");
                    break;

            }

        }

        #endif

        return;

    }

    tcp_socket tcp_socket::accept(void) {

        sockaddr_in client_address{

        };

        #ifdef _WIN32
        int addrlen = sizeof(client_address);
        #else
        socklen_t addrlen = sizeof(client_address);
        #endif

        auto client_handler = ::accept(socket_handler, reinterpret_cast<sockaddr*>(&client_address), &addrlen);

        #ifdef _WIN32
        if (client_handler == INVALID_SOCKET)
            switch (WSAGetLastError()) {

                case WSAECONNRESET:
                    throw std::runtime_error("Connection reset by peer.");
                    break;

                case WSAEINVAL:
                    throw std::runtime_error("Not listening on any port.");
                    break;

                case WSAENETDOWN:
                    throw std::runtime_error("Network module failure.");
                    break;

                case WSAEINPROGRESS:
                    throw std::runtime_error("A callback is in progres.");
                    break;

                case WSAEFAULT:
                    throw std::runtime_error("Address space is invalid.");
                    break;

                default:
                    throw std::runtime_error("Connection failed.");
                    break;

            }

        #else
        if (client_handler < 0)
            switch (errno) {

                case EAGAIN:
                #if EWOULDBLOCK != EAGAIN
                case EWOULDBLOCK:
                #endif

                    throw std::runtime_error(
                        "The socket is marked nonblocking and no connections are present to be accepted"
                    );
                    break;

                case ECONNABORTED:
                    throw std::runtime_error("Connection aborted.");
                    break;

                case EFAULT:
                    throw std::runtime_error("Address space is invalid.");
                    break;

                case EINTR:
                    throw std::runtime_error("Caught a signal before valid connection was handled.");
                    break;

                case ENOBUFS:
                case ENOMEM:
                    throw std::runtime_error("Socket buffer limit reached.");
                    break;

                case EOPNOTSUPP:
                    throw std::runtime_error("The referenced socket is not of type SOCK_STREAM.");
                    break;

                case EPERM:
                    throw std::runtime_error("Firewall rules forbid connection.");
                    break;

                default:
                    throw std::runtime_error("Connection failed.");
                    break;

            }

        #endif

        return tcp_socket(client_handler);

    }

    std::size_t tcp_socket::recv(void* buffer, std::size_t size) {

        #ifdef _WIN32
        int received = ::recv(socket_handler, static_cast<char*>(buffer), (size <= INT_MAX) ? size : INT_MAX, 0);

        if (received == SOCKET_ERROR)
            switch (WSAGetLastError()) {

                case WSAENETDOWN:
                    throw std::runtime_error("The network subsystem has failed.");
                    break;

                case WSAENOTCONN:
                    throw std::runtime_error("The socket is not connected.");
                    break;

                case WSAEINTR:
                    throw std::runtime_error("The payload was interrupted.");
                    break;

                case WSAEINPROGRESS:
                    throw std::runtime_error("A callback is in progress.");
                    break;

                case WSAENETRESET:
                    throw std::runtime_error("Connection broken due to keep-alive activity failure.");
                    break;

                case WSAEOPNOTSUPP:
                    throw std::runtime_error(
                        "MSG_OOB was specified, but the socket is not stream-style such as type SOCK_STREAM"
                    );
                    break;

                case WSAESHUTDOWN:
                    throw std::runtime_error(
                        "The socket has been shut down; it is not possible to receive on a socket after shutdown has been "
                        "invoked"
                    );
                    break;

                case WSAEWOULDBLOCK:
                    throw std::runtime_error("Cannot do a blocking operation (recv) on a non-blocking socket.");
                    break;

                case WSAEMSGSIZE:
                    throw std::runtime_error("Message was larger than buffer size.");
                    break;

                case WSAECONNABORTED:
                    throw std::runtime_error("Socket closed due to timeout or other issue.");
                    break;

                case WSAETIMEDOUT:
                    throw std::runtime_error("Peer Timeout.");
                    break;

                case WSAECONNRESET:
                    throw std::runtime_error("Socket closed: connection reset.");
                    break;

                default:
                    throw std::runtime_error("Recv failed.");
                    break;

            }

        #else
        ssize_t received;

        do {

            received = ::recv(socket_handler, buffer, size, 0);

        } while (received < 0 && errno == EINTR);

        if (received == -1)
            switch (errno) {

                case EAGAIN:
                #if EWOULDBLOCK != EAGAIN
                case EWOULDBLOCK:
                #endif
                    throw std::runtime_error("Cannot do a blocking operation (recv) on a non-blocking socket.");
                    break;

                case ECONNREFUSED:
                    throw std::runtime_error("Connection refused.");
                    break;

                case EFAULT:
                    throw std::runtime_error("The receive buffer pointer(s) point outside the process memory.");
                    break;

                case ENOTCONN:
                    throw std::runtime_error("Not connected to a peer.");
                    break;

                default:
                    throw std::runtime_error("Recv failed.");
                    break;

            }

        #endif

        return received;

    }

    void tcp_socket::send_all(const void* data, std::size_t size) {

        if (size == 0)
            return;

        if (data == nullptr)
            throw std::invalid_argument("Socket send failure: Null input buffer.");

        const auto* bytes = static_cast<const char*>(data);
        std::size_t sent_total = 0;

        while (sent_total < size) {

            const std::size_t remaining = size - sent_total;

            #ifdef _WIN32

            int res =
                ::send(socket_handler, bytes + sent_total, static_cast<int>((remaining <= INT_MAX) ? remaining : INT_MAX), 0);

            if (res == SOCKET_ERROR)
                switch (WSAGetLastError()) {

                    case WSAENETDOWN:
                        throw std::runtime_error("The network subsystem has failed.");
                        break;

                    case WSAENOTCONN:
                        throw std::runtime_error("The socket is not connected.");
                        break;

                    case WSAEINTR:
                        throw std::runtime_error("The payload was interrupted.");
                        break;

                    case WSAEINPROGRESS:
                        throw std::runtime_error("A callback is in progress.");
                        break;

                    case WSAENETRESET:
                        throw std::runtime_error("Connection broken due to keep-alive activity failure.");
                        break;

                    case WSAEOPNOTSUPP:
                        throw std::runtime_error(
                            "MSG_OOB was specified, but the socket is not stream-style such as type SOCK_STREAM"
                        );
                        break;

                    case WSAESHUTDOWN:
                        throw std::runtime_error(
                            "The socket has been shut down; it is not possible to send on a socket after shutdown has been "
                            "invoked"
                        );
                        break;

                    case WSAEWOULDBLOCK:
                        throw std::runtime_error("Cannot do a blocking operation (send) on a non-blocking socket.");
                        break;

                    case WSAEMSGSIZE:
                        throw std::runtime_error("Message was larger than buffer size.");
                        break;

                    case WSAECONNABORTED:
                        throw std::runtime_error("Socket closed due to timeout or other issue.");
                        break;

                    case WSAETIMEDOUT:
                        throw std::runtime_error("Peer Timeout.");
                        break;

                    case WSAECONNRESET:
                        throw std::runtime_error("Socket closed: connection reset.");
                        break;

                    default:
                        throw std::runtime_error("Send failed.");
                        break;

                }

            #else

            ssize_t res = ::send(
                socket_handler,
                bytes + sent_total,
                (remaining <= INT_MAX) ? remaining : static_cast<std::size_t>(INT_MAX),
                MSG_NOSIGNAL
            );

            if (res == -1)
                switch (errno) {

                    case EAGAIN:
                    #if EWOULDBLOCK != EAGAIN
                    case EWOULDBLOCK:
                    #endif
                        throw std::runtime_error("Cannot do a blocking operation (send) on a non-blocking socket.");
                        break;

                    case ECONNREFUSED:
                        throw std::runtime_error("Connection refused.");
                        break;

                    case EALREADY:
                        throw std::runtime_error("Another Fast Open is in progress.");
                        break;

                    case ECONNRESET:
                        throw std::runtime_error("Connection reset by peer.");
                        break;

                    case EFAULT:
                        throw std::runtime_error("The send buffer pointer(s) point outside the process memory.");
                        break;

                    case ENOTCONN:
                        throw std::runtime_error("Not connected to a peer.");
                        break;

                    case EINTR:
                        continue;

                    default:
                        throw std::runtime_error("Send failed.");
                        break;

                }

            #endif

            if (res == 0)
                throw std::runtime_error("Socket send failure: No progress while sending data.");

            sent_total += static_cast<std::size_t>(res);

        }

        return;

    };

    bool tcp_socket::wait_readable(int timeout_ms) {

        if (timeout_ms < 0)
            throw std::invalid_argument("Socket wait failure: Negative timeout.");

        #ifndef _WIN32
        if (socket_handler < 0 || socket_handler >= FD_SETSIZE)
            throw std::runtime_error("Socket wait failure: Descriptor outside select range.");

        #endif

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_handler, &readable);

        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;

        #ifdef _WIN32

        const int result = ::select(0, &readable, nullptr, nullptr, &timeout);

        if (result == SOCKET_ERROR) {

            if (WSAGetLastError() == WSAEINTR)
                return false;

            throw std::runtime_error("Socket wait failure.");

        }

        #else

        const int result = ::select(socket_handler + 1, &readable, nullptr, nullptr, &timeout);

        if (result == -1) {

            if (errno == EINTR)
                return false;

            throw std::runtime_error("Socket wait failure.");

        }

        #endif

        return result > 0;

    }

    void tcp_socket::interrupt() noexcept {

        #ifdef _WIN32

        if (socket_handler != INVALID_SOCKET)
            ::shutdown(socket_handler, SD_BOTH);

        #else

        if (socket_handler >= 0)
            ::shutdown(socket_handler, SHUT_RDWR);

        #endif

    }

}