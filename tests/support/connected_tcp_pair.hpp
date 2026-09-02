#pragma once

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "sparenode/network/tcp_listener.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sparenode::test
{

/// Identifies platform errors that mean IPv6 loopback is unavailable rather than broken.
/// @param[in] error Listener startup error produced while binding an IPv6 endpoint.
/// @return `true` only for native errors associated with unavailable IPv6 support.
[[nodiscard]] inline bool is_ipv6_loopback_unavailable(const network::NetworkError &error) noexcept
{
    if (error.domain == network::NetworkErrorDomain::address_resolution)
    {
        return error.code == EAI_FAMILY
#ifdef EAI_ADDRFAMILY
               || error.code == EAI_ADDRFAMILY
#endif
            ;
    }

#ifdef _WIN32
    return error.domain == network::NetworkErrorDomain::socket &&
           (error.code == WSAEAFNOSUPPORT || error.code == WSAEPROTONOSUPPORT ||
            error.code == WSAENOPROTOOPT || error.code == WSAEADDRNOTAVAIL);
#else
    return error.domain == network::NetworkErrorDomain::socket &&
           (error.code == EAFNOSUPPORT || error.code == EPROTONOSUPPORT ||
            error.code == ENOPROTOOPT || error.code == EADDRNOTAVAIL);
#endif
}

#ifdef _WIN32
using NativeTestSocket = SOCKET;
using TestSocketLength = int;
inline constexpr NativeTestSocket invalid_test_socket = INVALID_SOCKET;

inline void close_test_socket(const NativeTestSocket socket) noexcept
{
    if (socket != invalid_test_socket)
    {
        static_cast<void>(closesocket(socket));
    }
}
#else
using NativeTestSocket = int;
using TestSocketLength = socklen_t;
inline constexpr NativeTestSocket invalid_test_socket = -1;

inline void close_test_socket(const NativeTestSocket socket) noexcept
{
    if (socket != invalid_test_socket)
    {
        static_cast<void>(::close(socket));
    }
}
#endif

/// Owns the native client side of one loopback test connection.
class TestClientSocket final
{
  public:
    explicit TestClientSocket(const NativeTestSocket socket) noexcept : socket_(socket)
    {
    }

    ~TestClientSocket()
    {
        close_test_socket(socket_);
    }

    TestClientSocket(TestClientSocket &&other) noexcept
        : socket_(std::exchange(other.socket_, invalid_test_socket))
    {
    }

    TestClientSocket(const TestClientSocket &) = delete;
    TestClientSocket &operator=(const TestClientSocket &) = delete;
    TestClientSocket &operator=(TestClientSocket &&) = delete;

    /// Waits until the server side is closed and no buffered payload remains.
    [[nodiscard]] bool peer_closes_within(const std::chrono::milliseconds timeout) const noexcept
    {
        fd_set readable{};
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);

        const auto total_milliseconds = timeout.count();
        timeval native_timeout{
            static_cast<long>(total_milliseconds / 1000),
            static_cast<long>((total_milliseconds % 1000) * 1000),
        };
#ifdef _WIN32
        const int ready = ::select(0, &readable, nullptr, nullptr, &native_timeout);
#else
        const int ready = ::select(socket_ + 1, &readable, nullptr, nullptr, &native_timeout);
#endif
        if (ready != 1)
        {
            return false;
        }

        char byte{};
#ifdef _WIN32
        return ::recv(socket_, &byte, 1, MSG_PEEK) == 0;
#else
        return ::recv(socket_, &byte, sizeof(byte), MSG_PEEK) == 0;
#endif
    }

    /// Receives one caller-bounded payload from the connected server socket.
    [[nodiscard]] std::ptrdiff_t receive(const std::span<std::byte> bytes) const noexcept
    {
#ifdef _WIN32
        return ::recv(socket_, reinterpret_cast<char *>(bytes.data()),
                      static_cast<int>(bytes.size()), 0);
#else
        return ::recv(socket_, bytes.data(), bytes.size(), 0);
#endif
    }

    /// Waits for readability and then receives one caller-bounded payload.
    /// @param[out] bytes Storage receiving available server bytes.
    /// @param[in] timeout Maximum time spent waiting for readable data.
    /// @return Native receive count, or no value when the wait does not become readable.
    [[nodiscard]] std::optional<std::ptrdiff_t>
    receive_within(const std::span<std::byte> bytes,
                   const std::chrono::milliseconds timeout) const noexcept
    {
        fd_set readable{};
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);

        const auto total_milliseconds = timeout.count();
        timeval native_timeout{
            static_cast<long>(total_milliseconds / 1000),
            static_cast<long>((total_milliseconds % 1000) * 1000),
        };
#ifdef _WIN32
        const int ready = ::select(0, &readable, nullptr, nullptr, &native_timeout);
#else
        const int ready = ::select(socket_ + 1, &readable, nullptr, nullptr, &native_timeout);
#endif
        if (ready != 1)
        {
            return std::nullopt;
        }
        return receive(bytes);
    }

    /// Interrupts pending test I/O without releasing the native socket handle.
    void shutdown() const noexcept
    {
#ifdef _WIN32
        static_cast<void>(::shutdown(socket_, SD_BOTH));
#else
        static_cast<void>(::shutdown(socket_, SHUT_RDWR));
#endif
    }

  private:
    NativeTestSocket socket_{invalid_test_socket};
};

/// Connects an owning native test client to a public numeric endpoint.
/// @param[in] endpoint Loopback endpoint exposed by the server under test.
/// @return Connected client socket whose destructor closes the native handle.
[[nodiscard]] inline TestClientSocket connect_test_client(const network::TcpEndpoint &endpoint)
{
    const int address_family = endpoint.address.find(':') == std::string::npos ? AF_INET : AF_INET6;
    const NativeTestSocket socket = ::socket(address_family, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(socket != invalid_test_socket);
    TestClientSocket client(socket);

    if (address_family == AF_INET)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(endpoint.port);
        REQUIRE(inet_pton(AF_INET, endpoint.address.c_str(), &address.sin_addr) == 1);
        REQUIRE(::connect(socket, reinterpret_cast<const sockaddr *>(&address),
                          static_cast<TestSocketLength>(sizeof(address))) == 0);
    }
    else
    {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(endpoint.port);
        REQUIRE(inet_pton(AF_INET6, endpoint.address.c_str(), &address.sin6_addr) == 1);
        REQUIRE(::connect(socket, reinterpret_cast<const sockaddr *>(&address),
                          static_cast<TestSocketLength>(sizeof(address))) == 0);
    }

    return client;
}

/// Groups the public server side with the native client side of a loopback connection.
struct ConnectedTcpPair
{
    network::TcpConnection server;
    TestClientSocket client;
};

/// Creates a real loopback connection through the public listener API.
[[nodiscard]] inline ConnectedTcpPair create_connected_tcp_pair()
{
    auto listener_result = network::TcpListener::bind({"127.0.0.1", 0});
    REQUIRE(listener_result.has_value());
    auto listener = std::move(listener_result.value());

    const auto endpoint = listener.local_endpoint();
    REQUIRE(endpoint.has_value());

    const NativeTestSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(socket != invalid_test_socket);
    TestClientSocket client(socket);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint->port);
    REQUIRE(inet_pton(AF_INET, endpoint->address.c_str(), &address.sin_addr) == 1);
    REQUIRE(::connect(socket, reinterpret_cast<const sockaddr *>(&address),
                      static_cast<TestSocketLength>(sizeof(address))) == 0);

    auto connection_result = listener.accept();
    REQUIRE(connection_result.has_value());
    return {std::move(connection_result.value()), std::move(client)};
}

} // namespace sparenode::test
