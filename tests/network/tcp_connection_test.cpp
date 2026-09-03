#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <stop_token>
#include <string_view>
#include <utility>

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

void close_test_socket(const NativeTestSocket socket) noexcept
{
    if (socket != invalid_test_socket)
    {
        static_cast<void>(::close(socket));
    }
}
#endif

/// Owns the native client side of one loopback integration-test connection.
class TestSocket final
{
  public:
    explicit TestSocket(const NativeTestSocket socket) noexcept : socket_(socket)
    {
    }

    ~TestSocket()
    {
        close();
    }

    TestSocket(TestSocket &&other) noexcept
        : socket_(std::exchange(other.socket_, invalid_test_socket))
    {
    }

    TestSocket(const TestSocket &) = delete;
    TestSocket &operator=(const TestSocket &) = delete;
    TestSocket &operator=(TestSocket &&) = delete;

    /// Closes the client side early to test orderly peer shutdown.
    void close() noexcept
    {
        close_test_socket(std::exchange(socket_, invalid_test_socket));
    }

    /// Configures an abortive close so the server observes a real socket failure.
    [[nodiscard]] bool close_with_reset() noexcept
    {
        const linger reset{1, 0};
#ifdef _WIN32
        const int result =
            setsockopt(socket_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&reset),
                       static_cast<int>(sizeof(reset)));
#else
        const int result = setsockopt(socket_, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
#endif
        close();
        return result == 0;
    }

    /// Sends a small test payload through the native client socket.
    [[nodiscard]] std::ptrdiff_t send(const std::span<const std::byte> bytes) const noexcept
    {
#ifdef _WIN32
        return ::send(socket_, reinterpret_cast<const char *>(bytes.data()),
                      static_cast<int>(bytes.size()), 0);
#else
        return ::send(socket_, bytes.data(), bytes.size(), 0);
#endif
    }

    /// Receives a small test payload from the native client socket.
    [[nodiscard]] std::ptrdiff_t receive(const std::span<std::byte> bytes) const noexcept
    {
#ifdef _WIN32
        return ::recv(socket_, reinterpret_cast<char *>(bytes.data()),
                      static_cast<int>(bytes.size()), 0);
#else
        return ::recv(socket_, bytes.data(), bytes.size(), 0);
#endif
    }

  private:
    NativeTestSocket socket_{invalid_test_socket};
};

struct ConnectedPair
{
    sparenode::network::TcpConnection server;
    TestSocket client;
};

/// Creates a real loopback connection with public server and native client sides.
[[nodiscard]] ConnectedPair create_connected_pair()
{
    auto listener_result = sparenode::network::TcpListener::bind({"127.0.0.1", 0});
    REQUIRE(listener_result.has_value());
    auto listener = std::move(listener_result.value());

    const auto endpoint = listener.local_endpoint();
    REQUIRE(endpoint.has_value());

    const NativeTestSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(socket != invalid_test_socket);
    TestSocket client(socket);

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

/// Converts text bytes without allocating a transport-owned buffer.
[[nodiscard]] std::span<const std::byte> bytes_of(const std::string_view text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

void check_cancelled(const sparenode::network::NetworkError &error,
                     const sparenode::network::NetworkOperation operation)
{
    CHECK(error.operation == operation);
    CHECK(error.domain == sparenode::network::NetworkErrorDomain::cancellation);
    CHECK(error.code == 0);
}

} // namespace

TEST_CASE("TCP connection receives caller-bounded bytes", "[network][tcp][io]")
{
    auto pair = create_connected_pair();
    constexpr std::string_view payload = "hello from client";
    REQUIRE(pair.client.send(bytes_of(payload)) == static_cast<std::ptrdiff_t>(payload.size()));

    std::array<std::byte, 64> buffer{};
    const auto received = pair.server.receive(buffer);

    REQUIRE(received.has_value());
    REQUIRE(received.value() == payload.size());
    CHECK(std::string_view(reinterpret_cast<const char *>(buffer.data()), received.value()) ==
          payload);
}

TEST_CASE("TCP receive preserves caller buffer bounds", "[network][tcp][io][partial]")
{
    auto pair = create_connected_pair();
    constexpr std::string_view payload = "larger payload";
    REQUIRE(pair.client.send(bytes_of(payload)) == static_cast<std::ptrdiff_t>(payload.size()));

    std::array<std::byte, 4> buffer{};
    const auto received = pair.server.receive(buffer);

    REQUIRE(received.has_value());
    REQUIRE(received.value() == buffer.size());
    CHECK(std::string_view(reinterpret_cast<const char *>(buffer.data()), buffer.size()) ==
          payload.substr(0, buffer.size()));
}

TEST_CASE("TCP connection sends caller-bounded bytes", "[network][tcp][io]")
{
    auto pair = create_connected_pair();
    constexpr std::string_view payload = "hello from server";

    const auto sent = pair.server.send(bytes_of(payload));
    REQUIRE(sent.has_value());
    REQUIRE(sent.value() == payload.size());

    std::array<std::byte, 64> buffer{};
    const auto received = pair.client.receive(buffer);
    REQUIRE(received == static_cast<std::ptrdiff_t>(payload.size()));
    CHECK(std::string_view(reinterpret_cast<const char *>(buffer.data()), payload.size()) ==
          payload);
}

TEST_CASE("TCP receive reports orderly peer shutdown as zero bytes", "[network][tcp][io]")
{
    auto pair = create_connected_pair();
    pair.client.close();

    std::array<std::byte, 1> buffer{};
    const auto received = pair.server.receive(buffer);

    REQUIRE(received.has_value());
    CHECK(received.value() == 0);
    CHECK(pair.server.is_open());
}

TEST_CASE("TCP receive reports peer reset as a structured socket error",
          "[network][tcp][io][error]")
{
    auto pair = create_connected_pair();
    REQUIRE(pair.client.close_with_reset());

    std::array<std::byte, 1> buffer{};
    const auto received = pair.server.receive(buffer);

    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().operation == sparenode::network::NetworkOperation::receive);
    CHECK(received.error().domain == sparenode::network::NetworkErrorDomain::socket);
    CHECK(received.error().code != 0);
    CHECK(pair.server.is_open());
}

TEST_CASE("TCP connection I/O rejects empty buffers", "[network][tcp][io][validation]")
{
    auto pair = create_connected_pair();

    const auto received = pair.server.receive(std::span<std::byte>{});
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().operation == sparenode::network::NetworkOperation::receive);
    CHECK(received.error().domain == sparenode::network::NetworkErrorDomain::validation);

    const auto sent = pair.server.send(std::span<const std::byte>{});
    REQUIRE_FALSE(sent.has_value());
    CHECK(sent.error().operation == sparenode::network::NetworkOperation::send);
    CHECK(sent.error().domain == sparenode::network::NetworkErrorDomain::validation);
}

TEST_CASE("TCP connection I/O rejects moved-from state", "[network][tcp][io][state]")
{
    auto pair = create_connected_pair();
    auto connection = std::move(pair.server);
    std::array<std::byte, 1> buffer{};

    const auto received = pair.server.receive(buffer);
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().operation == sparenode::network::NetworkOperation::receive);
    CHECK(received.error().domain == sparenode::network::NetworkErrorDomain::state);

    const auto sent = pair.server.send(std::span<const std::byte>(buffer));
    REQUIRE_FALSE(sent.has_value());
    CHECK(sent.error().operation == sparenode::network::NetworkOperation::send);
    CHECK(sent.error().domain == sparenode::network::NetworkErrorDomain::state);
    CHECK(connection.is_open());
}

TEST_CASE("TCP connection I/O honours cancellation requested before waiting",
          "[network][tcp][io][cancel]")
{
    auto pair = create_connected_pair();
    std::stop_source stop_source;
    REQUIRE(stop_source.request_stop());
    std::array<std::byte, 1> buffer{};

    const auto received = pair.server.receive(buffer, stop_source.get_token());
    REQUIRE_FALSE(received.has_value());
    check_cancelled(received.error(), sparenode::network::NetworkOperation::receive);

    const auto sent = pair.server.send(std::span<const std::byte>(buffer), stop_source.get_token());
    REQUIRE_FALSE(sent.has_value());
    check_cancelled(sent.error(), sparenode::network::NetworkOperation::send);
    CHECK(pair.server.is_open());
}

TEST_CASE("TCP receive expires through the native monotonic deadline",
          "[network][tcp][io][timeout]")
{
    auto pair = create_connected_pair();
    std::array<std::byte, 1> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{25};

    const auto received = pair.server.receive(buffer, {.stop_token = {}, .deadline = deadline});

    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().operation == sparenode::network::NetworkOperation::receive);
    CHECK(received.error().domain == sparenode::network::NetworkErrorDomain::timeout);
    CHECK(received.error().code == 0);
    CHECK(pair.server.is_open());

    constexpr std::string_view payload = "x";
    const auto sent = pair.server.send(
        bytes_of(payload), {.stop_token = {}, .deadline = std::chrono::steady_clock::now()});
    REQUIRE_FALSE(sent.has_value());
    CHECK(sent.error().operation == sparenode::network::NetworkOperation::send);
    CHECK(sent.error().domain == sparenode::network::NetworkErrorDomain::timeout);
}

TEST_CASE("TCP receive succeeds when data arrives before its deadline",
          "[network][tcp][io][timeout]")
{
    auto pair = create_connected_pair();
    constexpr std::string_view payload = "ready";
    REQUIRE(pair.client.send(bytes_of(payload)) == static_cast<std::ptrdiff_t>(payload.size()));
    std::array<std::byte, 8> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};

    const auto received = pair.server.receive(buffer, {.stop_token = {}, .deadline = deadline});

    REQUIRE(received.has_value());
    CHECK(received.value() == payload.size());
}

TEST_CASE("TCP I/O gives pre-requested cancellation priority over an expired deadline",
          "[network][tcp][io][timeout][cancel]")
{
    auto pair = create_connected_pair();
    std::stop_source stop_source;
    REQUIRE(stop_source.request_stop());
    std::array<std::byte, 1> buffer{};

    const auto received =
        pair.server.receive(buffer, {.stop_token = stop_source.get_token(),
                                     .deadline = std::chrono::steady_clock::now()});

    REQUIRE_FALSE(received.has_value());
    check_cancelled(received.error(), sparenode::network::NetworkOperation::receive);
}
