#pragma once

#include <chrono>
#include <cstddef>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/network/tcp_listener.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sparenode::test
{

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

  private:
    NativeTestSocket socket_{invalid_test_socket};
};

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
