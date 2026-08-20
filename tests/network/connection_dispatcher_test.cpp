#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include "sparenode/network/connection_dispatcher.hpp"
#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/result.hpp"
#include "support/connected_tcp_pair.hpp"

namespace
{

namespace network = sparenode::network;

constexpr auto test_timeout = std::chrono::seconds{2};

/// Coordinates a blocking test handler without sleeps or busy-waiting.
class StopAwareGate final
{
  public:
    /// Waits until the test opens the gate or dispatcher shutdown is requested.
    [[nodiscard]] bool wait(const std::stop_token &stop_token)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait(lock, stop_token, [this] { return open_; });
    }

    /// Opens the gate permanently and wakes all test handlers.
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

/// Creates a dispatcher and moves its successful value out of Result storage.
[[nodiscard]] network::ConnectionDispatcher
create_dispatcher(network::ConnectionDispatcherConfig config)
{
    auto result = network::ConnectionDispatcher::create(std::move(config));
    REQUIRE(result.has_value());
    return std::move(result).value();
}

/// Returns a handler that immediately releases every submitted connection.
[[nodiscard]] sparenode::network::ConnectionHandler successful_handler()
{
    return [](network::TcpConnection,
              const std::stop_token &) -> sparenode::Result<void, network::NetworkError>
    { return {}; };
}

/// Returns an optional test value after enforcing its presence for analyzers.
template <typename Value> [[nodiscard]] Value &require_optional(std::optional<Value> &optional)
{
    if (!optional.has_value())
    {
        throw std::logic_error("Expected optional test value to be present");
    }
    return optional.value();
}

} // namespace

TEST_CASE("Connection dispatcher rejects invalid resource limits",
          "[network][dispatcher][validation]")
{
    auto zero_workers = network::ConnectionDispatcher::create({{0, 1}, successful_handler(), {}});
    REQUIRE_FALSE(zero_workers.has_value());
    CHECK(zero_workers.error() ==
          network::DispatchError{network::DispatchErrorCode::invalid_worker_count, 0});

    auto zero_capacity = network::ConnectionDispatcher::create({{1, 0}, successful_handler(), {}});
    REQUIRE_FALSE(zero_capacity.has_value());
    CHECK(zero_capacity.error() ==
          network::DispatchError{network::DispatchErrorCode::invalid_pending_connection_limit, 0});

    auto missing_handler = network::ConnectionDispatcher::create({{1, 1}, {}, {}});
    REQUIRE_FALSE(missing_handler.has_value());
    CHECK(missing_handler.error() ==
          network::DispatchError{network::DispatchErrorCode::missing_connection_handler, 0});
}

TEST_CASE("Connection dispatcher processes multiple connections concurrently",
          "[network][dispatcher][concurrency]")
{
    StopAwareGate gate;
    std::counting_semaphore<4> started(0);
    std::counting_semaphore<4> completed(0);
    std::atomic<int> active_count{};
    std::atomic<int> maximum_active_count{};

    auto handler =
        [&](network::TcpConnection,
            const std::stop_token &stop_token) -> sparenode::Result<void, network::NetworkError>
    {
        const int active = active_count.fetch_add(1) + 1;
        int observed_maximum = maximum_active_count.load();
        while (active > observed_maximum &&
               !maximum_active_count.compare_exchange_weak(observed_maximum, active))
        {
        }
        started.release();
        static_cast<void>(gate.wait(stop_token));
        active_count.fetch_sub(1);
        completed.release();
        return {};
    };

    auto dispatcher = create_dispatcher({{2, 2}, std::move(handler), {}});
    auto first = sparenode::test::create_connected_tcp_pair();
    auto second = sparenode::test::create_connected_tcp_pair();

    REQUIRE(dispatcher.submit(std::move(first.server), {}).has_value());
    REQUIRE(dispatcher.submit(std::move(second.server), {}).has_value());
    REQUIRE(started.try_acquire_for(test_timeout));
    REQUIRE(started.try_acquire_for(test_timeout));
    CHECK(maximum_active_count.load() == 2);

    gate.open();
    REQUIRE(completed.try_acquire_for(test_timeout));
    REQUIRE(completed.try_acquire_for(test_timeout));
    dispatcher.request_stop();
}

TEST_CASE("Connection dispatcher rejects invalid and pre-cancelled submissions",
          "[network][dispatcher][validation][cancel]")
{
    auto dispatcher = create_dispatcher({{1, 1}, successful_handler(), {}});
    auto invalid_pair = sparenode::test::create_connected_tcp_pair();
    auto retained_connection = std::move(invalid_pair.server);

    auto invalid_result = dispatcher.submit(std::move(invalid_pair.server), {});
    REQUIRE_FALSE(invalid_result.has_value());
    CHECK(invalid_result.error() ==
          network::DispatchError{network::DispatchErrorCode::invalid_connection, 0});
    CHECK(retained_connection.is_open());

    auto cancelled_pair = sparenode::test::create_connected_tcp_pair();
    std::stop_source stop_source;
    REQUIRE(stop_source.request_stop());
    auto cancelled_result =
        dispatcher.submit(std::move(cancelled_pair.server), stop_source.get_token());
    REQUIRE_FALSE(cancelled_result.has_value());
    CHECK(cancelled_result.error() ==
          network::DispatchError{network::DispatchErrorCode::cancelled, 0});
    dispatcher.request_stop();
}

TEST_CASE("Move construction transfers a running dispatcher", "[network][dispatcher][move]")
{
    std::binary_semaphore handled(0);
    auto handler = [&](network::TcpConnection,
                       const std::stop_token &) -> sparenode::Result<void, network::NetworkError>
    {
        handled.release();
        return {};
    };

    auto source = create_dispatcher({{1, 1}, std::move(handler), {}});
    auto destination = std::move(source);

    auto rejected = sparenode::test::create_connected_tcp_pair();
    // Exercising the documented moved-from contract is intentional.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    const auto rejected_result = source.submit(std::move(rejected.server), {});
    REQUIRE_FALSE(rejected_result.has_value());
    CHECK(rejected_result.error() ==
          network::DispatchError{network::DispatchErrorCode::stopped, 0});
    CHECK(rejected.client.peer_closes_within(
        std::chrono::duration_cast<std::chrono::milliseconds>(test_timeout)));

    auto accepted = sparenode::test::create_connected_tcp_pair();
    REQUIRE(destination.submit(std::move(accepted.server), {}).has_value());
    REQUIRE(handled.try_acquire_for(test_timeout));
    destination.request_stop();
}

TEST_CASE("Move assignment shuts down replaced dispatcher state",
          "[network][dispatcher][move][shutdown]")
{
    StopAwareGate gate;
    std::binary_semaphore original_handler_started(0);
    auto original_handler =
        [&](network::TcpConnection,
            const std::stop_token &stop_token) -> sparenode::Result<void, network::NetworkError>
    {
        original_handler_started.release();
        static_cast<void>(gate.wait(stop_token));
        return {};
    };

    auto destination = create_dispatcher({{1, 1}, std::move(original_handler), {}});
    auto active = sparenode::test::create_connected_tcp_pair();
    auto pending = sparenode::test::create_connected_tcp_pair();
    REQUIRE(destination.submit(std::move(active.server), {}).has_value());
    REQUIRE(original_handler_started.try_acquire_for(test_timeout));
    REQUIRE(destination.submit(std::move(pending.server), {}).has_value());

    std::binary_semaphore replacement_handled(0);
    auto replacement_handler =
        [&](network::TcpConnection,
            const std::stop_token &) -> sparenode::Result<void, network::NetworkError>
    {
        replacement_handled.release();
        return {};
    };
    auto replacement = create_dispatcher({{1, 1}, std::move(replacement_handler), {}});

    destination = std::move(replacement);

    CHECK(active.client.peer_closes_within(
        std::chrono::duration_cast<std::chrono::milliseconds>(test_timeout)));
    CHECK(pending.client.peer_closes_within(
        std::chrono::duration_cast<std::chrono::milliseconds>(test_timeout)));
    auto accepted = sparenode::test::create_connected_tcp_pair();
    REQUIRE(destination.submit(std::move(accepted.server), {}).has_value());
    REQUIRE(replacement_handled.try_acquire_for(test_timeout));
    destination.request_stop();
}

TEST_CASE("A full dispatcher queue supports caller cancellation",
          "[network][dispatcher][capacity][cancel]")
{
    StopAwareGate gate;
    std::binary_semaphore handler_started(0);
    auto handler =
        [&](network::TcpConnection,
            const std::stop_token &stop_token) -> sparenode::Result<void, network::NetworkError>
    {
        handler_started.release();
        static_cast<void>(gate.wait(stop_token));
        return {};
    };

    auto dispatcher = create_dispatcher({{1, 1}, std::move(handler), {}});
    auto active = sparenode::test::create_connected_tcp_pair();
    auto pending = sparenode::test::create_connected_tcp_pair();
    auto cancelled = sparenode::test::create_connected_tcp_pair();

    REQUIRE(dispatcher.submit(std::move(active.server), {}).has_value());
    REQUIRE(handler_started.try_acquire_for(test_timeout));
    REQUIRE(dispatcher.submit(std::move(pending.server), {}).has_value());

    std::optional<sparenode::Result<void, network::DispatchError>> submission_result;
    std::jthread producer(
        [&](const std::stop_token &stop_token)
        { submission_result.emplace(dispatcher.submit(std::move(cancelled.server), stop_token)); });
    producer.request_stop();
    producer.join();

    auto &completed_submission = require_optional(submission_result);
    REQUIRE_FALSE(completed_submission.has_value());
    CHECK(completed_submission.error() ==
          network::DispatchError{network::DispatchErrorCode::cancelled, 0});

    gate.open();
    dispatcher.request_stop();
}

TEST_CASE("Dispatcher shutdown wakes blocked producers and drops queued ownership",
          "[network][dispatcher][shutdown]")
{
    StopAwareGate gate;
    std::binary_semaphore handler_started(0);
    auto handler =
        [&](network::TcpConnection,
            const std::stop_token &stop_token) -> sparenode::Result<void, network::NetworkError>
    {
        handler_started.release();
        static_cast<void>(gate.wait(stop_token));
        return {};
    };

    auto dispatcher = create_dispatcher({{1, 1}, std::move(handler), {}});
    auto active = sparenode::test::create_connected_tcp_pair();
    auto pending = sparenode::test::create_connected_tcp_pair();
    auto blocked = sparenode::test::create_connected_tcp_pair();

    REQUIRE(dispatcher.submit(std::move(active.server), {}).has_value());
    REQUIRE(handler_started.try_acquire_for(test_timeout));
    REQUIRE(dispatcher.submit(std::move(pending.server), {}).has_value());

    std::binary_semaphore producer_started(0);
    std::optional<sparenode::Result<void, network::DispatchError>> submission_result;
    std::jthread producer(
        [&]
        {
            producer_started.release();
            submission_result.emplace(dispatcher.submit(std::move(blocked.server), {}));
        });
    REQUIRE(producer_started.try_acquire_for(test_timeout));

    dispatcher.request_stop();
    producer.join();

    auto &completed_submission = require_optional(submission_result);
    REQUIRE_FALSE(completed_submission.has_value());
    CHECK(completed_submission.error() ==
          network::DispatchError{network::DispatchErrorCode::stopped, 0});
    CHECK(pending.client.peer_closes_within(
        std::chrono::duration_cast<std::chrono::milliseconds>(test_timeout)));
    CHECK(blocked.client.peer_closes_within(
        std::chrono::duration_cast<std::chrono::milliseconds>(test_timeout)));
}

TEST_CASE("Dispatcher shutdown cancels active connection operations",
          "[network][dispatcher][shutdown][io]")
{
    std::binary_semaphore handler_started(0);
    std::binary_semaphore failure_observed(0);
    std::optional<network::ConnectionFailure> observed_failure;

    auto handler =
        [&](network::TcpConnection connection,
            const std::stop_token &stop_token) -> sparenode::Result<void, network::NetworkError>
    {
        handler_started.release();
        std::array<std::byte, 1> buffer{};
        auto received = connection.receive(buffer, stop_token);
        if (!received)
        {
            return sparenode::unexpected(received.error());
        }
        return {};
    };
    auto observer = [&](const network::ConnectionFailure &failure)
    {
        observed_failure = failure;
        failure_observed.release();
    };

    auto dispatcher = create_dispatcher({{1, 1}, std::move(handler), std::move(observer)});
    auto pair = sparenode::test::create_connected_tcp_pair();
    REQUIRE(dispatcher.submit(std::move(pair.server), {}).has_value());
    REQUIRE(handler_started.try_acquire_for(test_timeout));

    dispatcher.request_stop();
    REQUIRE(failure_observed.try_acquire_for(test_timeout));
    auto &failure = require_optional(observed_failure);
    CHECK(failure.kind == network::ConnectionFailureKind::handler_error);
    auto &network_error = require_optional(failure.network_error);
    CHECK(network_error.operation == network::NetworkOperation::receive);
    CHECK(network_error.domain == network::NetworkErrorDomain::cancellation);
}

TEST_CASE("Handler failures remain isolated from later connections",
          "[network][dispatcher][failure]")
{
    std::atomic<int> invocation_count{};
    std::binary_semaphore failure_observed(0);
    std::binary_semaphore success_observed(0);
    std::optional<network::ConnectionFailure> observed_failure;
    const network::NetworkError expected_error{network::NetworkOperation::receive,
                                               network::NetworkErrorDomain::socket, 42};

    auto handler = [&](network::TcpConnection,
                       const std::stop_token &) -> sparenode::Result<void, network::NetworkError>
    {
        if (invocation_count.fetch_add(1) == 0)
        {
            return sparenode::unexpected(expected_error);
        }
        success_observed.release();
        return {};
    };
    auto observer = [&](const network::ConnectionFailure &failure)
    {
        observed_failure = failure;
        failure_observed.release();
    };

    auto dispatcher = create_dispatcher({{1, 2}, std::move(handler), std::move(observer)});
    auto first = sparenode::test::create_connected_tcp_pair();
    auto second = sparenode::test::create_connected_tcp_pair();
    REQUIRE(dispatcher.submit(std::move(first.server), {}).has_value());
    REQUIRE(dispatcher.submit(std::move(second.server), {}).has_value());

    REQUIRE(failure_observed.try_acquire_for(test_timeout));
    REQUIRE(success_observed.try_acquire_for(test_timeout));
    auto &failure = require_optional(observed_failure);
    CHECK(failure.kind == network::ConnectionFailureKind::handler_error);
    CHECK(failure.network_error == expected_error);
    CHECK(invocation_count.load() == 2);
    dispatcher.request_stop();
}

TEST_CASE("Handler and observer exceptions cannot terminate dispatcher workers",
          "[network][dispatcher][exception]")
{
    std::atomic<int> invocation_count{};
    std::binary_semaphore second_connection_completed(0);
    auto handler = [&](network::TcpConnection,
                       const std::stop_token &) -> sparenode::Result<void, network::NetworkError>
    {
        if (invocation_count.fetch_add(1) == 0)
        {
            throw std::runtime_error("test handler failure");
        }
        second_connection_completed.release();
        return {};
    };
    auto throwing_observer = [](const network::ConnectionFailure &)
    { throw std::runtime_error("test observer failure"); };

    auto dispatcher = create_dispatcher({{1, 2}, std::move(handler), std::move(throwing_observer)});
    auto first = sparenode::test::create_connected_tcp_pair();
    auto second = sparenode::test::create_connected_tcp_pair();
    REQUIRE(dispatcher.submit(std::move(first.server), {}).has_value());
    REQUIRE(dispatcher.submit(std::move(second.server), {}).has_value());

    REQUIRE(second_connection_completed.try_acquire_for(test_timeout));
    CHECK(invocation_count.load() == 2);
    dispatcher.request_stop();
}
