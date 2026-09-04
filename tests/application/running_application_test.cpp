#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <utility>

#include "sparenode/application/running_application.hpp"
#include "sparenode/configuration/runtime/share_config.hpp"
#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/result.hpp"
#include "support/connected_tcp_pair.hpp"
#include "support/optional.hpp"
#include "support/temporary_directory.hpp"

TEST_CASE("Running application starts from runtime settings and retains shares",
          "[application][startup]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-application");
    auto root_result = sparenode::configuration::SharedRoot::create(directory.path());
    REQUIRE(root_result.has_value());

    std::vector<sparenode::configuration::runtime::ShareConfig> shares;
    shares.emplace_back("Documents", std::move(root_result).value(),
                        sparenode::configuration::runtime::SharePermissions{true, false, false});
    std::vector<sparenode::configuration::runtime::ServerConfig> servers;
    servers.emplace_back(sparenode::network::TcpEndpoint{"127.0.0.1", 0}, false, 1,
                         sparenode::logging::LogSeverity::info, std::move(shares));
    sparenode::configuration::runtime::AppConfig config(std::move(servers));

    auto handler = [](sparenode::network::TcpConnection, const std::stop_token &)
        -> sparenode::Result<void, sparenode::network::NetworkError> { return {}; };
    auto result = sparenode::application::RunningApplication::start(std::move(config), handler);

    REQUIRE(result.has_value());
    REQUIRE(result->servers().size() == 1);
    const auto endpoint = result->servers().front().local_endpoint();
    const auto &local_endpoint = sparenode::test::require_optional(endpoint);
    CHECK(local_endpoint.address == "127.0.0.1");
    CHECK(local_endpoint.port != 0);
    REQUIRE(result->config().servers().front().shares().size() == 1);
    CHECK(result->config().servers().front().shares().front().root().path() ==
          std::filesystem::canonical(directory.path()));
}

TEST_CASE("Running application rejects an empty runtime server collection",
          "[application][startup]")
{
    auto handler = [](sparenode::network::TcpConnection, const std::stop_token &)
        -> sparenode::Result<void, sparenode::network::NetworkError> { return {}; };
    const auto result = sparenode::application::RunningApplication::start({}, handler);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == sparenode::application::ApplicationStartErrorCode::missing_server);
}

TEST_CASE("Running application installs connection failure observers on its dispatchers",
          "[application][startup][logging]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-application-observer");
    auto root_result = sparenode::configuration::SharedRoot::create(directory.path());
    REQUIRE(root_result.has_value());
    std::vector<sparenode::configuration::runtime::ShareConfig> shares;
    shares.emplace_back("Documents", std::move(root_result).value(),
                        sparenode::configuration::runtime::SharePermissions{true, false, false});
    std::vector<sparenode::configuration::runtime::ServerConfig> servers;
    servers.emplace_back(sparenode::network::TcpEndpoint{"127.0.0.1", 0}, false, 1,
                         sparenode::logging::LogSeverity::info, std::move(shares));

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
    auto result = sparenode::application::RunningApplication::start(
        sparenode::configuration::runtime::AppConfig(std::move(servers)), std::move(handler),
        {std::move(observer), {}});
    REQUIRE(result.has_value());
    const auto endpoint = result->servers().front().local_endpoint();
    auto client = sparenode::test::connect_test_client(sparenode::test::require_optional(endpoint));

    REQUIRE(observed_signal.try_acquire_for(std::chrono::seconds{1}));
    const auto &failure = sparenode::test::require_optional(observed_failure);
    CHECK(failure.kind == sparenode::network::ConnectionFailureKind::handler_error);
    const auto &network_error = sparenode::test::require_optional(failure.network_error);
    CHECK(network_error.operation == expected_error.operation);
    CHECK(network_error.domain == expected_error.domain);
    CHECK(network_error.code == expected_error.code);
    CHECK(client.peer_closes_within(std::chrono::seconds{1}));
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
