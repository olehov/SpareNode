#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <utility>

#include "sparenode/network/network_error.hpp"
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
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{

#ifdef _WIN32
using NativeTestSocket = SOCKET;
using TestSocketLength = int;
constexpr NativeTestSocket invalid_test_socket = INVALID_SOCKET;

// Releases a Windows client socket created only for an integration test.
void close_test_socket(const NativeTestSocket socket) noexcept
{
    if (socket != invalid_test_socket)
    {
        static_cast<void>(closesocket(socket));
    }
}
#else
using NativeTestSocket = int;
using TestSocketLength = socklen_t;
constexpr NativeTestSocket invalid_test_socket = -1;

// Releases a POSIX client socket created only for an integration test.
void close_test_socket(const NativeTestSocket socket) noexcept
{
    if (socket != invalid_test_socket)
    {
        static_cast<void>(::close(socket));
    }
}
#endif

class TestSocket final
{
  public:
    // Takes ownership immediately so a failed REQUIRE cannot leak the socket.
    explicit TestSocket(const NativeTestSocket socket) noexcept : socket_(socket)
    {
    }

    // Releases the native test socket at the end of a test scope.
    ~TestSocket()
    {
        // Keep test failures and early returns from leaking the native client socket.
        close_test_socket(socket_);
    }

    // Transfers ownership and marks the source as invalid to prevent a double close.
    TestSocket(TestSocket &&other) noexcept
        : socket_(std::exchange(other.socket_, invalid_test_socket))
    {
    }

    TestSocket &operator=(TestSocket &&) = delete;
    TestSocket(const TestSocket &) = delete;
    TestSocket &operator=(const TestSocket &) = delete;

    // Reports whether the helper currently owns a native socket.
    [[nodiscard]] bool is_open() const noexcept
    {
        return socket_ != invalid_test_socket;
    }

    // Borrows the native handle for connect without transferring ownership.
    [[nodiscard]] NativeTestSocket native_handle() const noexcept
    {
        return socket_;
    }

  private:
    NativeTestSocket socket_{invalid_test_socket};
};

// Creates a real IPv4 client and connects it to the listener under test.
[[nodiscard]] TestSocket connect_to(const sparenode::network::TcpEndpoint &endpoint)
{
    const NativeTestSocket socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(socket_handle != invalid_test_socket);
    TestSocket client(socket_handle);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    REQUIRE(inet_pton(AF_INET, endpoint.address.c_str(), &address.sin_addr) == 1);

    const int connect_result =
        ::connect(client.native_handle(), reinterpret_cast<const sockaddr *>(&address),
                  static_cast<TestSocketLength>(sizeof(address)));
    REQUIRE(connect_result == 0);

    return client;
}

} // namespace

TEST_CASE("TCP listener rejects non-numeric interfaces", "[network][tcp]")
{
    // AI_NUMERICHOST makes this fail without performing a DNS lookup.
    const auto listener = sparenode::network::TcpListener::bind({"localhost", 0});

    REQUIRE_FALSE(listener.has_value());
    CHECK(listener.error().operation == sparenode::network::NetworkOperation::resolve_address);
    CHECK(listener.error().domain == sparenode::network::NetworkErrorDomain::address_resolution);
}

TEST_CASE("TCP listener accepts a loopback connection", "[network][tcp]")
{
    // Port zero avoids races with a hard-coded port already used on the test machine.
    auto listener_result = sparenode::network::TcpListener::bind({"127.0.0.1", 0});
    REQUIRE(listener_result.has_value());

    // Moving transfers the socket and leaves the value stored in Result closed.
    auto listener = std::move(listener_result.value());
    REQUIRE(listener.is_open());
    CHECK_FALSE(listener_result->is_open());

    const auto local_endpoint = listener.local_endpoint();
    REQUIRE(local_endpoint.has_value());
    CHECK(local_endpoint->address == "127.0.0.1");
    REQUIRE(local_endpoint->port != 0);

    auto client = connect_to(local_endpoint.value());
    REQUIRE(client.is_open());

    auto connection_result = listener.accept();
    REQUIRE(connection_result.has_value());
    REQUIRE(connection_result->is_open());

    const auto peer_endpoint = connection_result->peer_endpoint();
    REQUIRE(peer_endpoint.has_value());
    const auto peer = peer_endpoint.value_or(sparenode::network::TcpEndpoint{});
    CHECK(peer.address == "127.0.0.1");
    CHECK(peer.port != 0);

    auto connection = std::move(connection_result.value());
    CHECK(connection.is_open());
    CHECK_FALSE(connection_result->is_open());
}

TEST_CASE("TCP listener releases its port on destruction", "[network][tcp]")
{
    std::uint16_t released_port = 0;
    {
        // Leaving this scope destroys the first listener and closes its socket.
        auto first_listener = sparenode::network::TcpListener::bind({"127.0.0.1", 0});
        REQUIRE(first_listener.has_value());

        const auto endpoint = first_listener->local_endpoint();
        REQUIRE(endpoint.has_value());
        released_port = endpoint->port;
    }

    // A successful second bind proves that RAII released the original port.
    const auto second_listener =
        sparenode::network::TcpListener::bind({"127.0.0.1", released_port});
    REQUIRE(second_listener.has_value());
}
