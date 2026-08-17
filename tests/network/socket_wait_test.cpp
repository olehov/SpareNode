#include <catch2/catch_test_macros.hpp>

#include <future>
#include <stop_token>
#include <thread>

#include "sparenode/network/detail/socket_wait.hpp"
#include "support/fake_socket_poller.hpp"

namespace
{

using WaitResult = sparenode::Result<sparenode::network::detail::SocketWaitStatus,
                                     sparenode::network::NetworkError>;

/// Verifies that either readiness interest can be interrupted through the wake entry.
void check_cancellable_wait(const sparenode::network::detail::SocketWaitInterest interest,
                            const sparenode::network::NetworkOperation operation)
{
    const auto runtime_result = sparenode::network::detail::ensure_socket_runtime();
    REQUIRE(runtime_result.has_value());
    sparenode::test::FakeSocketPoller poller;
    std::stop_source stop_source;
    std::promise<WaitResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread wait_thread(
        [&poller, &stop_source, &result_promise, interest, operation]
        {
            result_promise.set_value(sparenode::network::detail::wait_for_socket(
                sparenode::network::detail::invalid_socket, interest, operation,
                stop_source.get_token(), poller));
        });

    poller.wait_until_entered();
    REQUIRE(stop_source.request_stop());
    poller.complete_with_readable(1);

    result_future.wait();
    const auto result = result_future.get();
    wait_thread.join();

    REQUIRE(result.has_value());
    CHECK(result.value() == sparenode::network::detail::SocketWaitStatus::cancelled);
    CHECK(poller.operation() == operation);
    CHECK(poller.entry_count() == 2);
}

} // namespace

TEST_CASE("Socket wait selects readable readiness", "[network][tcp][io][unit]")
{
    sparenode::test::FakeSocketPoller poller;
    std::promise<WaitResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread wait_thread(
        [&poller, &result_promise]
        {
            result_promise.set_value(sparenode::network::detail::wait_for_socket(
                sparenode::network::detail::invalid_socket,
                sparenode::network::detail::SocketWaitInterest::readable,
                sparenode::network::NetworkOperation::receive, poller));
        });

    poller.wait_until_entered();
    poller.complete_with_readable(0);
    result_future.wait();
    const auto result = result_future.get();
    wait_thread.join();

    REQUIRE(result.has_value());
    CHECK(result.value() == sparenode::network::detail::SocketWaitStatus::socket_ready);
    CHECK(poller.watches_readable());
    CHECK_FALSE(poller.watches_writable());
}

TEST_CASE("Socket wait selects writable readiness", "[network][tcp][io][unit]")
{
    sparenode::test::FakeSocketPoller poller;
    std::promise<WaitResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread wait_thread(
        [&poller, &result_promise]
        {
            result_promise.set_value(sparenode::network::detail::wait_for_socket(
                sparenode::network::detail::invalid_socket,
                sparenode::network::detail::SocketWaitInterest::writable,
                sparenode::network::NetworkOperation::send, poller));
        });

    poller.wait_until_entered();
    poller.complete_with_writable(0);
    result_future.wait();
    const auto result = result_future.get();
    wait_thread.join();

    REQUIRE(result.has_value());
    CHECK(result.value() == sparenode::network::detail::SocketWaitStatus::socket_ready);
    CHECK_FALSE(poller.watches_readable());
    CHECK(poller.watches_writable());
}

TEST_CASE("Non-stoppable socket wait does not create a wake channel", "[network][tcp][io][unit]")
{
    sparenode::test::FakeSocketPoller poller;
    std::promise<WaitResult> result_promise;
    auto result_future = result_promise.get_future();

    std::jthread wait_thread(
        [&poller, &result_promise]
        {
            result_promise.set_value(sparenode::network::detail::wait_for_socket(
                sparenode::network::detail::invalid_socket,
                sparenode::network::detail::SocketWaitInterest::readable,
                sparenode::network::NetworkOperation::receive, std::stop_token{}, poller));
        });

    poller.wait_until_entered();
    poller.complete_with_readable(0);
    result_future.wait();
    const auto result = result_future.get();
    wait_thread.join();

    REQUIRE(result.has_value());
    CHECK(poller.entry_count() == 1);
}

TEST_CASE("Readable socket wait is cancellable", "[network][tcp][io][cancel][unit]")
{
    check_cancellable_wait(sparenode::network::detail::SocketWaitInterest::readable,
                           sparenode::network::NetworkOperation::receive);
}

TEST_CASE("Writable socket wait is cancellable", "[network][tcp][io][cancel][unit]")
{
    check_cancellable_wait(sparenode::network::detail::SocketWaitInterest::writable,
                           sparenode::network::NetworkOperation::send);
}
