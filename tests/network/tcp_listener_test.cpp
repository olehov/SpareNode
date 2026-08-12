#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstdint>
#include <utility>

#include "sparenode/network/detail/retry_interrupted_operation.hpp"
#include "sparenode/network/detail/socket_address.hpp"
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
#include <netdb.h>
#include <netinet/in.h>
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

// Identifies platform errors that mean IPv6 is disabled rather than broken.
[[nodiscard]] bool is_ipv6_unavailable(const sparenode::network::NetworkError &error) noexcept
{
    if (error.domain == sparenode::network::NetworkErrorDomain::address_resolution)
    {
        return error.code == EAI_FAMILY
#ifdef EAI_ADDRFAMILY
               || error.code == EAI_ADDRFAMILY
#endif
            ;
    }

    return error.domain == sparenode::network::NetworkErrorDomain::socket &&
           (error.code == WSAEAFNOSUPPORT || error.code == WSAEPROTONOSUPPORT ||
            error.code == WSAENOPROTOOPT || error.code == WSAEADDRNOTAVAIL);
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

// Identifies platform errors that mean IPv6 is disabled rather than broken.
[[nodiscard]] bool is_ipv6_unavailable(const sparenode::network::NetworkError &error) noexcept
{
    if (error.domain == sparenode::network::NetworkErrorDomain::address_resolution)
    {
        return error.code == EAI_FAMILY
#ifdef EAI_ADDRFAMILY
               || error.code == EAI_ADDRFAMILY
#endif
            ;
    }

    return error.domain == sparenode::network::NetworkErrorDomain::socket &&
           (error.code == EAFNOSUPPORT || error.code == EPROTONOSUPPORT ||
            error.code == ENOPROTOOPT || error.code == EADDRNOTAVAIL);
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

TEST_CASE("TCP listener rejects invalid configuration", "[network][tcp]")
{
    const auto empty_address = sparenode::network::TcpListener::bind({"", 0});
    REQUIRE_FALSE(empty_address.has_value());
    CHECK(empty_address.error().operation == sparenode::network::NetworkOperation::bind);
    CHECK(empty_address.error().domain == sparenode::network::NetworkErrorDomain::validation);

    const auto invalid_backlog = sparenode::network::TcpListener::bind({"127.0.0.1", 0}, 0);
    REQUIRE_FALSE(invalid_backlog.has_value());
    CHECK(invalid_backlog.error().operation == sparenode::network::NetworkOperation::bind);
    CHECK(invalid_backlog.error().domain == sparenode::network::NetworkErrorDomain::validation);
}

TEST_CASE("TCP listener binds an IPv6 loopback interface", "[network][tcp]")
{
    auto listener = sparenode::network::TcpListener::bind({"::1", 0});
    if (!listener && is_ipv6_unavailable(listener.error()))
    {
        SKIP("IPv6 loopback is unavailable on this host");
    }
    REQUIRE(listener.has_value());

    const auto endpoint = listener->local_endpoint();
    REQUIRE(endpoint.has_value());
    CHECK(endpoint->address == "::1");
    CHECK(endpoint->port != 0);
}

TEST_CASE("IPv6 endpoint conversion preserves its numeric scope", "[network][tcp][ipv6]")
{
    sockaddr_in6 scoped_address{};
    scoped_address.sin6_family = AF_INET6;
    scoped_address.sin6_port = htons(4242);
    scoped_address.sin6_scope_id = 7;
    REQUIRE(inet_pton(AF_INET6, "fe80::1", &scoped_address.sin6_addr) == 1);

    const auto endpoint = sparenode::network::detail::endpoint_from_address(
        reinterpret_cast<const sockaddr *>(&scoped_address),
        sparenode::network::NetworkOperation::query_local_endpoint);

    REQUIRE(endpoint.has_value());
    CHECK(endpoint->address == "fe80::1%7");
    CHECK(endpoint->port == 4242);
}

#ifndef _WIN32
TEST_CASE("Interrupted accept operation is retried deterministically", "[network][tcp][posix]")
{
    int operation_calls = 0;
    const auto result = sparenode::network::detail::retry_interrupted_operation(
        invalid_test_socket,
        [&operation_calls]
        {
            ++operation_calls;
            return operation_calls == 1 ? invalid_test_socket : NativeTestSocket{42};
        },
        [] { return EINTR; }, sparenode::network::detail::should_retry_accept);

    CHECK(operation_calls == 2);
    CHECK(result.value == 42);
    CHECK(result.error_code == 0);
}
#endif

TEST_CASE("TCP listener accepts a loopback connection", "[network][tcp]")
{
    // Port zero avoids races with a hard-coded port already used on the test machine.
    auto listener_result = sparenode::network::TcpListener::bind({"127.0.0.1", 0});
    REQUIRE(listener_result.has_value());

    // Moving transfers the socket and leaves the value stored in Result closed.
    auto listener = std::move(listener_result.value());
    REQUIRE(listener.is_open());
    CHECK_FALSE(listener_result->is_open());

    const auto moved_from_endpoint = listener_result->local_endpoint();
    REQUIRE_FALSE(moved_from_endpoint.has_value());
    CHECK(moved_from_endpoint.error().operation ==
          sparenode::network::NetworkOperation::query_local_endpoint);
    CHECK(moved_from_endpoint.error().domain == sparenode::network::NetworkErrorDomain::state);

    const auto moved_from_accept = listener_result->accept();
    REQUIRE_FALSE(moved_from_accept.has_value());
    CHECK(moved_from_accept.error().operation == sparenode::network::NetworkOperation::accept);
    CHECK(moved_from_accept.error().domain == sparenode::network::NetworkErrorDomain::state);

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

        // A second listener cannot take the address while the first still owns it.
        const auto conflicting_listener =
            sparenode::network::TcpListener::bind({"127.0.0.1", released_port});
        CHECK_FALSE(conflicting_listener.has_value());
    }

    // A successful second bind proves that RAII released the original port.
    const auto second_listener =
        sparenode::network::TcpListener::bind({"127.0.0.1", released_port});
    REQUIRE(second_listener.has_value());
}
