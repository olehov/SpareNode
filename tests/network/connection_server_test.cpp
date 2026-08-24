#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <stop_token>
#include <utility>

#include "sparenode/network/connection_dispatcher.hpp"
#include "sparenode/network/connection_server.hpp"
#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/result.hpp"
#include "support/connected_tcp_pair.hpp"
#include "support/optional.hpp"

namespace
{

namespace network = sparenode::network;

constexpr auto test_timeout = std::chrono::seconds{2};

/// @brief Coordinates a blocking integration-test handler without polling.
class StopAwareGate final
{
  public:
    /// @brief Waits until the test opens the gate or server shutdown is requested.
    /// @param[in] stop_token Worker token requested by server shutdown.
    /// @return `true` when opened, or `false` when cancellation won.
    [[nodiscard]] bool wait(const std::stop_token &stop_token)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait(lock, stop_token, [this] { return open_; });
    }

    /// @brief Opens the gate permanently and wakes every waiting handler.
    void open()
    {
        {
            std::scoped_lock lock(mutex_);
            open_ = true;
        }
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable_any condition_;
    bool open_{};
};

/// @brief Starts a loopback server and unwraps the result for integration tests.
/// @param[in] dispatcher_config Fixed worker limits and connection callbacks.
/// @param[in] failure_observer Optional accept-loop failure observer.
/// @return Running server bound to an operating-system-selected port.
[[nodiscard]] network::ConnectionServer
start_server(network::ConnectionDispatcherConfig dispatcher_config,
             network::ConnectionServerFailureObserver failure_observer = {})
{
    auto server = network::ConnectionServer::start(
        {{"127.0.0.1", 0}, 128, true, std::move(dispatcher_config), std::move(failure_observer)});
    REQUIRE(server.has_value());
    return std::move(server).value();
}

/// @brief Returns a cancellation error suitable for a stopped test handler.
/// @return Structured receive cancellation error.
[[nodiscard]] network::NetworkError cancellation_error() noexcept
{
    return {network::NetworkOperation::receive, network::NetworkErrorDomain::cancellation, 0};
}

/// @brief Blocks the first invocation while recording completion of the second.
class BlockingFirstHandler final
{
  public:
    /// @brief Handles one connection according to its invocation order.
    /// @param[in] connection Connection owned only for the duration of this call.
    /// @param[in] stop_token Worker token requested during server shutdown.
    /// @return Success after the gate opens, or cancellation during shutdown.
    [[nodiscard]] sparenode::Result<void, network::NetworkError>
    handle(network::TcpConnection connection, const std::stop_token &stop_token)
    {
        static_cast<void>(connection);
        if (invocation_count_.fetch_add(1) == 0)
        {
            first_started_.release();
            if (!gate_.wait(stop_token))
            {
                return sparenode::unexpected(cancellation_error());
            }
            first_completed_.store(true);
        }
        else
        {
            second_overlapped_first_.store(!first_completed_.load());
            second_completed_.release();
        }
        return {};
    }

    /// @brief Waits for the first handler invocation.
    /// @return `true` when the first handler started before the test timeout.
    [[nodiscard]] bool wait_for_first()
    {
        return first_started_.try_acquire_for(test_timeout);
    }

    /// @brief Waits for the second handler invocation.
    /// @return `true` when the second handler completed before the test timeout.
    [[nodiscard]] bool wait_for_second()
    {
        return second_completed_.try_acquire_for(test_timeout);
    }

    /// @brief Releases the first blocked invocation.
    void release_first()
    {
        gate_.open();
    }

    /// @brief Returns the number of started handler invocations.
    /// @return Thread-safe invocation count.
    [[nodiscard]] int invocation_count() const noexcept
    {
        return invocation_count_.load();
    }

    /// @brief Reports whether the second invocation overlapped the blocked first invocation.
    /// @return `true` when more than one worker entered the handler concurrently.
    [[nodiscard]] bool second_overlapped_first() const noexcept
    {
        return second_overlapped_first_.load();
    }

  private:
    StopAwareGate gate_;
    std::atomic<int> invocation_count_{};
    std::atomic<bool> first_completed_{};
    std::atomic<bool> second_overlapped_first_{};
    std::binary_semaphore first_started_{0};
    std::binary_semaphore second_completed_{0};
};

/// @brief Blocks in receive and records the dispatcher failure produced on shutdown.
class CancellableReceiveHandler final
{
  public:
    /// @brief Receives one byte until data arrives or shutdown cancels the operation.
    /// @param[in] connection Connection used for the cancellable receive.
    /// @param[in] stop_token Worker token requested during server shutdown.
    /// @return Receive success or its structured network error.
    [[nodiscard]] sparenode::Result<void, network::NetworkError>
    handle(network::TcpConnection connection, const std::stop_token &stop_token)
    {
        started_.release();
        std::byte byte{};
        auto result = connection.receive(std::span<std::byte>(&byte, 1), stop_token);
        if (!result)
        {
            return sparenode::unexpected(result.error());
        }
        return {};
    }

    /// @brief Stores one dispatcher failure and releases the observing test thread.
    /// @param[in] failure Failure isolated by the dispatcher.
    void observe(const network::ConnectionFailure &failure)
    {
        failure_ = failure;
        failure_observed_.release();
    }

    /// @brief Waits for the handler to enter receive.
    /// @return `true` when the handler started before the test timeout.
    [[nodiscard]] bool wait_until_started()
    {
        return started_.try_acquire_for(test_timeout);
    }

    /// @brief Waits for the dispatcher failure observer.
    /// @return `true` when a failure arrived before the test timeout.
    [[nodiscard]] bool wait_for_failure()
    {
        return failure_observed_.try_acquire_for(test_timeout);
    }

    /// @brief Checks the complete cancellation failure expected after shutdown.
    /// @return `true` when the recorded failure describes receive cancellation.
    [[nodiscard]] bool has_expected_failure() const
    {
        if (!failure_.has_value())
        {
            return false;
        }
        const auto &failure = failure_.value();
        return failure.kind == network::ConnectionFailureKind::handler_error &&
               failure.network_error == cancellation_error();
    }

  private:
    std::binary_semaphore started_{0};
    std::binary_semaphore failure_observed_{0};
    std::optional<network::ConnectionFailure> failure_;
};

/// @brief Fails the first client and records successful handling of the next one.
class IsolatedFailureHandler final
{
  public:
    /// @brief Returns a deterministic error once and succeeds thereafter.
    /// @param[in] connection Connection owned only for the duration of this call.
    /// @param[in] stop_token Unused worker cancellation token.
    /// @return First-call error or later-call success.
    [[nodiscard]] sparenode::Result<void, network::NetworkError>
    handle(network::TcpConnection connection, const std::stop_token &stop_token)
    {
        static_cast<void>(connection);
        static_cast<void>(stop_token);
        if (invocation_count_.fetch_add(1) == 0)
        {
            return sparenode::unexpected(expected_error());
        }
        second_completed_.release();
        return {};
    }

    /// @brief Stores the isolated failure and wakes the test thread.
    /// @param[in] failure Failure reported by the dispatcher.
    void observe(const network::ConnectionFailure &failure)
    {
        failure_ = failure;
        failure_observed_.release();
    }

    /// @brief Returns the deterministic handler error used by this helper.
    /// @return Structured socket error used for the first invocation.
    [[nodiscard]] static network::NetworkError expected_error() noexcept
    {
        return {network::NetworkOperation::receive, network::NetworkErrorDomain::socket, 42};
    }

    /// @brief Waits for the isolated first-client failure.
    /// @return `true` when the observer ran before the test timeout.
    [[nodiscard]] bool wait_for_failure()
    {
        return failure_observed_.try_acquire_for(test_timeout);
    }

    /// @brief Waits for successful handling of the second client.
    /// @return `true` when the second handler completed before the test timeout.
    [[nodiscard]] bool wait_for_second()
    {
        return second_completed_.try_acquire_for(test_timeout);
    }

    /// @brief Checks the complete first-client failure recorded by the observer.
    /// @return `true` when the expected handler error was observed.
    [[nodiscard]] bool has_expected_failure() const
    {
        if (!failure_.has_value())
        {
            return false;
        }
        const auto &failure = failure_.value();
        return failure.kind == network::ConnectionFailureKind::handler_error &&
               failure.network_error == expected_error();
    }

    /// @brief Returns the number of started handler invocations.
    /// @return Thread-safe invocation count.
    [[nodiscard]] int invocation_count() const noexcept
    {
        return invocation_count_.load();
    }

  private:
    std::atomic<int> invocation_count_{};
    std::binary_semaphore failure_observed_{0};
    std::binary_semaphore second_completed_{0};
    std::optional<network::ConnectionFailure> failure_;
};

} // namespace

TEST_CASE("Connection server reports listener and dispatcher startup failures",
          "[network][server][validation]")
{
    auto handler = [](network::TcpConnection,
                      const std::stop_token &) -> sparenode::Result<void, network::NetworkError>
    { return {}; };

    auto invalid_listener =
        network::ConnectionServer::start({{"", 0}, 128, true, {{1, 1}, handler, {}}, {}});
    REQUIRE_FALSE(invalid_listener.has_value());
    const auto &listener_error = invalid_listener.error();
    CHECK(listener_error.code == network::ConnectionServerStartErrorCode::listener_start_failed);
    const auto &network_error = sparenode::test::require_optional(listener_error.network_error);
    CHECK(network_error.domain == network::NetworkErrorDomain::validation);

    auto invalid_dispatcher = network::ConnectionServer::start(
        {{"127.0.0.1", 0}, 128, true, {{0, 1}, std::move(handler), {}}, {}});
    REQUIRE_FALSE(invalid_dispatcher.has_value());
    const auto &server_error = invalid_dispatcher.error();
    CHECK(server_error.code == network::ConnectionServerStartErrorCode::dispatcher_start_failed);
    const auto &dispatch_error = sparenode::test::require_optional(server_error.dispatch_error);
    CHECK(dispatch_error.code == network::DispatchErrorCode::invalid_worker_count);
}

TEST_CASE("Connection server handles a second client while the first is blocked",
          "[network][server][concurrency]")
{
    BlockingFirstHandler handler_state;
    auto handler =
        [&handler_state](network::TcpConnection connection, const std::stop_token &stop_token)
    { return handler_state.handle(std::move(connection), stop_token); };

    auto server = start_server({{2, 2}, std::move(handler), {}});
    const auto endpoint = server.local_endpoint();
    const auto &local_endpoint = sparenode::test::require_optional(endpoint);

    auto first_client = sparenode::test::connect_test_client(local_endpoint);
    REQUIRE(handler_state.wait_for_first());
    auto second_client = sparenode::test::connect_test_client(local_endpoint);
    REQUIRE(handler_state.wait_for_second());

    handler_state.release_first();
    server.request_stop();
    CHECK(first_client.peer_closes_within(test_timeout));
    CHECK(second_client.peer_closes_within(test_timeout));
    CHECK(handler_state.invocation_count() == 2);
}

TEST_CASE("Connection server shutdown cancels active connection operations",
          "[network][server][shutdown][cancel]")
{
    CancellableReceiveHandler handler_state;
    auto handler =
        [&handler_state](network::TcpConnection connection, const std::stop_token &stop_token)
    { return handler_state.handle(std::move(connection), stop_token); };
    auto observer = [&handler_state](const network::ConnectionFailure &failure)
    { handler_state.observe(failure); };

    auto server = start_server({{1, 1}, std::move(handler), std::move(observer)});
    const auto endpoint = server.local_endpoint();
    const auto &local_endpoint = sparenode::test::require_optional(endpoint);
    auto client = sparenode::test::connect_test_client(local_endpoint);
    REQUIRE(handler_state.wait_until_started());

    server.request_stop();
    REQUIRE(handler_state.wait_for_failure());
    CHECK(handler_state.has_expected_failure());
    CHECK(client.peer_closes_within(test_timeout));
}

TEST_CASE("Connection server multithreading switch can enforce one worker",
          "[network][server][concurrency][configuration]")
{
    BlockingFirstHandler handler_state;
    auto handler =
        [&handler_state](network::TcpConnection connection, const std::stop_token &stop_token)
    { return handler_state.handle(std::move(connection), stop_token); };

    network::ConnectionServerConfig config{
        {"127.0.0.1", 0}, 128, false, {{2, 2}, std::move(handler), {}}, {}};
    REQUIRE(config.effective_worker_count() == 1);

    auto server_result = network::ConnectionServer::start(std::move(config));
    REQUIRE(server_result.has_value());
    auto server = std::move(server_result).value();
    const auto endpoint = server.local_endpoint();
    const auto &local_endpoint = sparenode::test::require_optional(endpoint);

    auto first_client = sparenode::test::connect_test_client(local_endpoint);
    REQUIRE(handler_state.wait_for_first());
    auto second_client = sparenode::test::connect_test_client(local_endpoint);

    handler_state.release_first();
    REQUIRE(handler_state.wait_for_second());
    server.request_stop();
    CHECK(first_client.peer_closes_within(test_timeout));
    CHECK(second_client.peer_closes_within(test_timeout));
    CHECK(handler_state.invocation_count() == 2);
    CHECK_FALSE(handler_state.second_overlapped_first());
}

TEST_CASE("Connection server accepts an IPv6 loopback client", "[network][server][ipv6]")
{
    std::binary_semaphore handled{0};
    auto handler =
        [&handled](network::TcpConnection,
                   const std::stop_token &) -> sparenode::Result<void, network::NetworkError>
    {
        handled.release();
        return {};
    };

    auto server_result = network::ConnectionServer::start(
        {{"::1", 0}, 128, false, {{1, 1}, std::move(handler), {}}, {}});
    if (!server_result.has_value())
    {
        const auto &network_error = server_result.error().network_error;
        if (network_error.has_value() &&
            sparenode::test::is_ipv6_loopback_unavailable(network_error.value()))
        {
            SKIP("IPv6 loopback is unavailable on this host");
        }
    }
    REQUIRE(server_result.has_value());
    auto server = std::move(server_result).value();
    const auto endpoint = server.local_endpoint();
    const auto &local_endpoint = sparenode::test::require_optional(endpoint);
    REQUIRE(local_endpoint.address == "::1");

    auto client = sparenode::test::connect_test_client(local_endpoint);
    REQUIRE(handled.try_acquire_for(test_timeout));

    server.request_stop();
    CHECK(client.peer_closes_within(test_timeout));
}

TEST_CASE("Connection server isolates one client failure from later clients",
          "[network][server][failure]")
{
    IsolatedFailureHandler handler_state;
    auto handler =
        [&handler_state](network::TcpConnection connection, const std::stop_token &stop_token)
    { return handler_state.handle(std::move(connection), stop_token); };
    auto observer = [&handler_state](const network::ConnectionFailure &failure)
    { handler_state.observe(failure); };

    auto server = start_server({{1, 2}, std::move(handler), std::move(observer)});
    const auto endpoint = server.local_endpoint();
    const auto &local_endpoint = sparenode::test::require_optional(endpoint);

    auto first_client = sparenode::test::connect_test_client(local_endpoint);
    REQUIRE(handler_state.wait_for_failure());
    CHECK(handler_state.has_expected_failure());
    auto second_client = sparenode::test::connect_test_client(local_endpoint);
    REQUIRE(handler_state.wait_for_second());

    server.request_stop();
    CHECK(first_client.peer_closes_within(test_timeout));
    CHECK(second_client.peer_closes_within(test_timeout));
    CHECK(handler_state.invocation_count() == 2);
}

TEST_CASE("Connection server destruction wakes an idle accept loop", "[network][server][shutdown]")
{
    auto handler = [](network::TcpConnection,
                      const std::stop_token &) -> sparenode::Result<void, network::NetworkError>
    { return {}; };

    {
        auto server = start_server({{1, 1}, std::move(handler), {}});
        REQUIRE(server.local_endpoint().has_value());
    }

    SUCCEED("Idle accept loop joined during destruction");
}
