#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <semaphore>
#include <span>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/application/running_application.hpp"
#include "sparenode/foundation/result.hpp"
#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "support/connected_tcp_pair.hpp"
#include "support/optional.hpp"

namespace
{

constexpr std::chrono::seconds test_timeout{1};

/// @brief Selects the independently variable dependencies of one test server plan.
struct TestPlanOptions
{
    sparenode::network::ConnectionFailureObserver observer{};
    std::optional<sparenode::network::ConnectionAdmissionOptions> admission{std::nullopt};
    sparenode::network::TcpEndpoint endpoint{"127.0.0.1", 0};
    bool multithreading_enabled{false};
    std::size_t worker_count{1};
};

/// @brief Creates one independently composed loopback runtime plan for tests.
[[nodiscard]] sparenode::application::ServerRuntimePlan
make_plan(sparenode::network::ConnectionHandler handler, TestPlanOptions options = {})
{
    return sparenode::application::ServerRuntimePlan(
        sparenode::network::ConnectionServerConfig{std::move(options.endpoint),
                                                   128,
                                                   options.multithreading_enabled,
                                                   {{options.worker_count, 128},
                                                    std::move(handler),
                                                    std::move(options.observer),
                                                    std::move(options.admission)},
                                                   {}});
}

/// @brief Returns a handler that accepts and immediately releases one connection.
[[nodiscard]] sparenode::network::ConnectionHandler successful_handler()
{
    return [](sparenode::network::TcpConnection,
              const std::stop_token &) -> sparenode::Result<void, sparenode::network::NetworkError>
    { return {}; };
}

} // namespace

TEST_CASE("Running application starts from one composed server plan", "[application][startup]")
{
    std::vector<sparenode::application::ServerRuntimePlan> plans;
    plans.push_back(make_plan(successful_handler()));

    auto result = sparenode::application::RunningApplication::start(std::move(plans));

    REQUIRE(result.has_value());
    REQUIRE(result->servers().size() == 1);
    const auto endpoint = result->servers().front().local_endpoint();
    const auto &local_endpoint = sparenode::test::require_optional(endpoint);
    CHECK(local_endpoint.address == "127.0.0.1");
    CHECK(local_endpoint.port != 0);
}

TEST_CASE("Running application rejects an empty runtime plan collection", "[application][startup]")
{
    const auto result = sparenode::application::RunningApplication::start({});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == sparenode::application::ApplicationStartErrorCode::missing_server);
}

TEST_CASE("Running application retains each plan's handler and admission policy",
          "[application][startup][isolation]")
{
    std::binary_semaphore first_handled{0};
    std::binary_semaphore second_handled{0};
    auto first_handler = [&first_handled](sparenode::network::TcpConnection,
                                          const std::stop_token &)
        -> sparenode::Result<void, sparenode::network::NetworkError>
    {
        first_handled.release();
        return {};
    };
    auto second_handler = [&second_handled](sparenode::network::TcpConnection,
                                            const std::stop_token &)
        -> sparenode::Result<void, sparenode::network::NetworkError>
    {
        second_handled.release();
        return {};
    };
    const sparenode::network::ConnectionAdmissionOptions second_admission{
        .max_prefix_bytes = 64,
        .completion_marker = "\r\n\r\n",
        .inactivity_timeout = test_timeout,
        .total_timeout = test_timeout};

    std::vector<sparenode::application::ServerRuntimePlan> plans;
    plans.push_back(make_plan(std::move(first_handler)));
    plans.push_back(make_plan(std::move(second_handler), {.admission = second_admission}));
    auto result = sparenode::application::RunningApplication::start(std::move(plans));
    REQUIRE(result);
    REQUIRE(result->servers().size() == 2);

    auto first_client = sparenode::test::connect_test_client(
        sparenode::test::require_optional(result->servers()[0].local_endpoint()));
    REQUIRE(first_handled.try_acquire_for(test_timeout));
    CHECK(first_client.peer_closes_within(test_timeout));
    CHECK_FALSE(second_handled.try_acquire_for(std::chrono::milliseconds{50}));

    auto second_client = sparenode::test::connect_test_client(
        sparenode::test::require_optional(result->servers()[1].local_endpoint()));
    CHECK_FALSE(second_handled.try_acquire_for(std::chrono::milliseconds{50}));
    constexpr std::string_view complete_prefix = "GET / HTTP/1.1\r\n\r\n";
    const auto prefix_bytes =
        std::as_bytes(std::span(complete_prefix.data(), complete_prefix.size()));
    REQUIRE(second_client.send(prefix_bytes) ==
            static_cast<std::ptrdiff_t>(complete_prefix.size()));
    REQUIRE(second_handled.try_acquire_for(test_timeout));
    CHECK_FALSE(first_handled.try_acquire_for(std::chrono::milliseconds{50}));
    CHECK(second_client.peer_closes_within(test_timeout));
}

TEST_CASE("Running application releases started servers after a later plan fails",
          "[application][startup][rollback]")
{
    // The first server's dispatcher owns this token through its handler until rollback completes.
    auto first_server_lifetime = std::make_shared<std::byte>();
    const std::weak_ptr<std::byte> first_server_released = first_server_lifetime;
    auto first_handler =
        [lifetime = std::move(first_server_lifetime)](
            sparenode::network::TcpConnection,
            const std::stop_token &) -> sparenode::Result<void, sparenode::network::NetworkError>
    {
        static_cast<void>(lifetime);
        return {};
    };

    std::vector<sparenode::application::ServerRuntimePlan> plans;
    plans.push_back(make_plan(std::move(first_handler)));
    plans.push_back(
        make_plan(successful_handler(), {.multithreading_enabled = true, .worker_count = 0}));
    const auto result = sparenode::application::RunningApplication::start(std::move(plans));
    REQUIRE_FALSE(result);
    CHECK(result.error().code ==
          sparenode::application::ApplicationStartErrorCode::server_start_failed);
    CHECK(result.error().server_index == 1);
    CHECK(first_server_released.expired());
}

TEST_CASE("Running application installs a plan's connection failure observer",
          "[application][startup][logging]")
{
    constexpr sparenode::network::NetworkError expected_error{
        sparenode::network::NetworkOperation::receive,
        sparenode::network::NetworkErrorDomain::state, 37};
    auto handler = [expected_error](sparenode::network::TcpConnection, const std::stop_token &)
        -> sparenode::Result<void, sparenode::network::NetworkError>
    { return sparenode::unexpected(expected_error); };
    std::binary_semaphore observed_signal{0};
    std::optional<sparenode::network::ConnectionFailure> observed_failure;
    auto observer =
        [&observed_signal, &observed_failure](const sparenode::network::ConnectionFailure &failure)
    {
        observed_failure = failure;
        observed_signal.release();
    };
    std::vector<sparenode::application::ServerRuntimePlan> plans;
    plans.push_back(make_plan(std::move(handler), {.observer = std::move(observer)}));

    auto result = sparenode::application::RunningApplication::start(std::move(plans));
    REQUIRE(result.has_value());
    const auto endpoint = result->servers().front().local_endpoint();
    auto client = sparenode::test::connect_test_client(sparenode::test::require_optional(endpoint));

    REQUIRE(observed_signal.try_acquire_for(test_timeout));
    const auto &failure = sparenode::test::require_optional(observed_failure);
    CHECK(failure.kind == sparenode::network::ConnectionFailureKind::handler_error);
    const auto &network_error = sparenode::test::require_optional(failure.network_error);
    CHECK(network_error.operation == expected_error.operation);
    CHECK(network_error.domain == expected_error.domain);
    CHECK(network_error.code == expected_error.code);
    CHECK(client.peer_closes_within(test_timeout));
}

TEST_CASE("Application startup diagnostics preserve nested server failure details",
          "[application][startup][errors]")
{
    const sparenode::application::ApplicationStartError error{
        sparenode::application::ApplicationStartErrorCode::server_start_failed, 2,
        sparenode::network::ConnectionServerStartError{
            sparenode::network::ConnectionServerStartErrorCode::listener_start_failed,
            sparenode::network::NetworkError{sparenode::network::NetworkOperation::bind,
                                             sparenode::network::NetworkErrorDomain::socket, 42},
            std::nullopt, 7}};

    CHECK(sparenode::application::format_application_start_error(error) ==
          "configured server could not be started server_index=2 server_error_code=0 "
          "native_code=7 network_operation=4 network_domain=2 network_code=42");
}

TEST_CASE("Application startup diagnostics preserve dispatcher failure details",
          "[application][startup][errors]")
{
    const sparenode::application::ApplicationStartError error{
        sparenode::application::ApplicationStartErrorCode::server_start_failed, 3,
        sparenode::network::ConnectionServerStartError{
            sparenode::network::ConnectionServerStartErrorCode::dispatcher_start_failed,
            std::nullopt,
            sparenode::network::DispatchError{
                sparenode::network::DispatchErrorCode::worker_start_failed, 87},
            0}};

    CHECK(sparenode::application::format_application_start_error(error) ==
          "configured server could not be started server_index=3 server_error_code=1 "
          "native_code=0 dispatch_code=6 dispatch_native_code=87");
}
