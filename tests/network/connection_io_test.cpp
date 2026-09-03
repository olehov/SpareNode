#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>

#include "sparenode/network/detail/connection_io.hpp"
#include "support/fake_socket_operations.hpp"
#include "support/fake_socket_poller.hpp"

namespace
{

using TransferResult = sparenode::Result<std::size_t, sparenode::network::NetworkError>;

[[nodiscard]] std::span<const std::byte> bytes_of(const std::string_view text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

[[nodiscard]] int would_block_error() noexcept
{
#ifdef _WIN32
    return WSAEWOULDBLOCK;
#else
    return EWOULDBLOCK;
#endif
}

/// Creates the production I/O coordinator with deterministic test collaborators.
[[nodiscard]] sparenode::network::detail::ConnectionIo
connection_io(sparenode::test::FakeSocketPoller &poller,
              sparenode::network::detail::SocketWakeChannel &wake_channel,
              sparenode::test::FakeSocketOperations &operations)
{
    return sparenode::network::detail::ConnectionIo({
        .wait =
            {
                .socket = sparenode::network::detail::invalid_socket,
                .poller = poller,
                .wake_channel = wake_channel,
            },
        .operations = operations,
    });
}

/// Enters the production receive/send wait before requesting cancellation.
void check_blocked_transfer_cancellation(const bool receive_operation)
{
    const auto runtime_result = sparenode::network::detail::ensure_socket_runtime();
    REQUIRE(runtime_result.has_value());

    sparenode::test::FakeSocketPoller poller;
    sparenode::test::FakeSocketOperations operations;
    sparenode::network::detail::SocketWakeChannel wake_channel;
    auto io = connection_io(poller, wake_channel, operations);
    std::stop_source stop_source;
    std::array<std::byte, 8> buffer{};
    std::promise<TransferResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread transfer_thread(
        [&]
        {
            if (receive_operation)
            {
                result_promise.set_value(io.receive(buffer, stop_source.get_token()));
            }
            else
            {
                result_promise.set_value(
                    io.send(std::span<const std::byte>(buffer), stop_source.get_token()));
            }
        });

    // This signal is emitted by the injected poller only after production code
    // has entered its readiness wait.
    poller.wait_until_entered();
    REQUIRE(stop_source.request_stop());
    poller.complete_with_readable(1);

    result_future.wait();
    const auto result = result_future.get();
    transfer_thread.join();

    REQUIRE_FALSE(result.has_value());
    const auto expected_operation = receive_operation
                                        ? sparenode::network::NetworkOperation::receive
                                        : sparenode::network::NetworkOperation::send;
    CHECK(result.error().operation == expected_operation);
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::cancellation);
    CHECK(operations.receive_calls() == 0);
    CHECK(operations.send_calls() == 0);
}

} // namespace

TEST_CASE("Connection send returns a controlled partial native transfer",
          "[network][tcp][io][unit][partial]")
{
    sparenode::test::FakeSocketPoller poller;
    sparenode::test::FakeSocketOperations operations;
    sparenode::network::detail::SocketWakeChannel wake_channel;
    auto io = connection_io(poller, wake_channel, operations);
    constexpr std::string_view payload = "partial send";
    constexpr std::ptrdiff_t partial_size = 4;
    operations.set_send_result(partial_size);
    std::promise<TransferResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread transfer_thread(
        [&] { result_promise.set_value(io.send(bytes_of(payload), std::stop_token{})); });

    poller.wait_until_entered();
    poller.complete_with_writable(0);
    result_future.wait();
    const auto result = result_future.get();
    transfer_thread.join();

    REQUIRE(result.has_value());
    CHECK(result.value() > 0);
    CHECK(result.value() < payload.size());
    CHECK(result.value() == static_cast<std::size_t>(partial_size));
    CHECK(std::ranges::equal(operations.sent_bytes(), bytes_of(payload.substr(0, result.value()))));
    CHECK(operations.send_calls() == 1);
    CHECK_FALSE(wake_channel.is_initialized());
}

TEST_CASE("Fake socket transfers cannot exceed their buffers",
          "[network][tcp][io][unit][support][bounds]")
{
    sparenode::test::FakeSocketOperations operations;
    std::array<std::byte, 3> receive_buffer{};
    constexpr std::string_view payload = "send";
    constexpr std::ptrdiff_t oversized_result = 64;

    operations.set_receive_result(oversized_result);
    const auto received =
        operations.receive(sparenode::network::detail::invalid_socket, receive_buffer);

    operations.set_send_result(oversized_result);
    const auto payload_bytes = bytes_of(payload);
    const auto sent = operations.send(sparenode::network::detail::invalid_socket, payload_bytes);

    CHECK(received == static_cast<std::ptrdiff_t>(receive_buffer.size()));
    CHECK(sent == static_cast<std::ptrdiff_t>(payload_bytes.size()));
    CHECK(std::ranges::equal(operations.sent_bytes(), payload_bytes));
}

TEST_CASE("Blocked connection receive cancellation is deterministic",
          "[network][tcp][io][unit][cancel]")
{
    check_blocked_transfer_cancellation(true);
}

TEST_CASE("Blocked connection send cancellation is deterministic",
          "[network][tcp][io][unit][cancel]")
{
    check_blocked_transfer_cancellation(false);
}

TEST_CASE("Connection I/O maps deadline expiry without attempting a transfer",
          "[network][tcp][io][unit][timeout]")
{
    sparenode::test::FakeSocketPoller poller;
    sparenode::test::FakeSocketOperations operations;
    sparenode::network::detail::SocketWakeChannel wake_channel;
    auto io = connection_io(poller, wake_channel, operations);
    std::array<std::byte, 8> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::hours{1};
    std::promise<TransferResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread transfer_thread(
        [&]
        {
            result_promise.set_value(
                io.receive_with_options(buffer, {.stop_token = {}, .deadline = deadline}));
        });
    poller.wait_until_entered();
    poller.complete_with_timeout();

    const auto result = result_future.get();
    transfer_thread.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().operation == sparenode::network::NetworkOperation::receive);
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::timeout);
    CHECK(result.error().code == 0);
    CHECK(operations.receive_calls() == 0);

    std::promise<TransferResult> send_promise;
    auto send_future = send_promise.get_future();
    std::jthread send_thread(
        [&]
        {
            send_promise.set_value(io.send_with_options(std::span<const std::byte>(buffer),
                                                        {.stop_token = {}, .deadline = deadline}));
        });
    poller.wait_until_entered();
    poller.complete_with_timeout();

    const auto sent = send_future.get();
    send_thread.join();
    REQUIRE_FALSE(sent.has_value());
    CHECK(sent.error().operation == sparenode::network::NetworkOperation::send);
    CHECK(sent.error().domain == sparenode::network::NetworkErrorDomain::timeout);
    CHECK(sent.error().code == 0);
    CHECK(operations.send_calls() == 0);
}

TEST_CASE("Connection receive does not retry would-block after hangup readiness",
          "[network][tcp][io][unit][error]")
{
    sparenode::test::FakeSocketPoller poller;
    sparenode::test::FakeSocketOperations operations;
    sparenode::network::detail::SocketWakeChannel wake_channel;
    auto io = connection_io(poller, wake_channel, operations);
    operations.set_receive_result(-1);
    operations.set_error_code(would_block_error());
    std::array<std::byte, 1> buffer{};
    std::promise<TransferResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread transfer_thread(
        [&] { result_promise.set_value(io.receive(buffer, std::stop_token{})); });

    poller.wait_until_entered();
    poller.complete_with_hangup(0);
    result_future.wait();
    const auto result = result_future.get();
    transfer_thread.join();

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().operation == sparenode::network::NetworkOperation::receive);
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::socket);
    CHECK(result.error().code == would_block_error());
    CHECK(operations.receive_calls() == 1);
}

TEST_CASE("Connection send does not retry would-block after error readiness",
          "[network][tcp][io][unit][error]")
{
    sparenode::test::FakeSocketPoller poller;
    sparenode::test::FakeSocketOperations operations;
    sparenode::network::detail::SocketWakeChannel wake_channel;
    auto io = connection_io(poller, wake_channel, operations);
    operations.set_send_result(-1);
    operations.set_error_code(would_block_error());
    std::array<std::byte, 1> buffer{};
    std::promise<TransferResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread transfer_thread(
        [&] {
            result_promise.set_value(
                io.send(std::span<const std::byte>(buffer), std::stop_token{}));
        });

    poller.wait_until_entered();
    poller.complete_with_socket_error(0);
    result_future.wait();
    const auto result = result_future.get();
    transfer_thread.join();

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().operation == sparenode::network::NetworkOperation::send);
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::socket);
    CHECK(result.error().code == would_block_error());
    CHECK(operations.send_calls() == 1);
}
