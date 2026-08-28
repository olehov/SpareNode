#include <catch2/catch_test_macros.hpp>

#include <stop_token>
#include <utility>

#include "sparenode/application/running_application.hpp"
#include "sparenode/configuration/runtime/share_config.hpp"
#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/network/network_error.hpp"
#include "sparenode/network/tcp_connection.hpp"
#include "sparenode/result.hpp"
#include "support/optional.hpp"
#include "support/temporary_directory.hpp"

TEST_CASE("Running application starts from runtime settings and retains shares",
          "[application][startup]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-application");
    auto root_result = sparenode::configuration::SharedRoot::create(directory.path());
    REQUIRE(root_result.has_value());

    sparenode::configuration::runtime::ServerConfig server;
    server.endpoint = {"127.0.0.1", 0};
    server.shares.push_back({"Documents", std::move(root_result).value(), {true, false, false}});
    sparenode::configuration::runtime::AppConfig config;
    config.servers.push_back(std::move(server));

    auto handler = [](sparenode::network::TcpConnection, const std::stop_token &)
        -> sparenode::Result<void, sparenode::network::NetworkError> { return {}; };
    auto result = sparenode::application::RunningApplication::start(std::move(config), handler);

    REQUIRE(result.has_value());
    REQUIRE(result->servers().size() == 1);
    const auto endpoint = result->servers().front().local_endpoint();
    const auto &local_endpoint = sparenode::test::require_optional(endpoint);
    CHECK(local_endpoint.address == "127.0.0.1");
    CHECK(local_endpoint.port != 0);
    REQUIRE(result->config().servers.front().shares.size() == 1);
    CHECK(result->config().servers.front().shares.front().root.path() ==
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
