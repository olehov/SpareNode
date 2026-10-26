#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "sparenode/authentication/credential_file_provisioner.hpp"
#include "sparenode/authentication/sensitive_password.hpp"
#include "sparenode/authentication/sodium_password_verifier.hpp"
#include "sparenode/bootstrap/runtime_bootstrap.hpp"
#include "sparenode/configuration/config_loader.hpp"
#include "sparenode/filesystem/location_permissions.hpp"
#include "sparenode/http/transport/http_transport_policy.hpp"
#include "sparenode/logging/log_severity.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "support/optional.hpp"
#include "support/temporary_directory.hpp"

namespace
{

[[nodiscard]] std::filesystem::path write_file(const std::filesystem::path &path,
                                               const std::string_view source)
{
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    REQUIRE(output.good());
    return path;
}

void provision_credentials(const std::filesystem::path &path)
{
    auto verifier = sparenode::authentication::SodiumPasswordVerifier::create();
    REQUIRE(verifier.has_value());
    auto provisioned = sparenode::authentication::CredentialFileProvisioner::provision(
        path, "owner", sparenode::authentication::SensitivePassword("bootstrap password"),
        verifier.value());
    REQUIRE(provisioned.has_value());
}

[[nodiscard]] std::string configuration_source(const std::filesystem::path &root,
                                               const std::string_view server_directives = {},
                                               const std::string_view location_directives = {})
{
    return "server {\n" + std::string(server_directives) +
           "location \"/api/Documents\" {\npath \"" + root.generic_string() + "\";\n" +
           std::string(location_directives) + "}\n}";
}

[[nodiscard]] sparenode::configuration::LoadedConfiguration
load_configuration(const std::filesystem::path &path)
{
    auto loaded = sparenode::configuration::ConfigLoader::load(path.string());
    REQUIRE(loaded.has_value());
    return std::move(loaded).value();
}

} // namespace

TEST_CASE("Runtime bootstrap constructs equivalent default runtime settings",
          "[bootstrap][configuration]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-bootstrap-defaults");
    const auto shared = directory.path() / "shared";
    REQUIRE(std::filesystem::create_directory(shared));
    provision_credentials(directory.path() / "credentials.conf");
    const auto config_path =
        write_file(directory.path() / "spnode.conf", configuration_source(shared));

    auto runtime = sparenode::bootstrap::RuntimeBootstrap::build(load_configuration(config_path));

    REQUIRE(runtime.has_value());
    REQUIRE(runtime->sessions() != nullptr);
    CHECK(runtime->session_authentication()->sessions() == runtime->sessions());
    auto session = runtime->session_authentication()->login(
        "owner", sparenode::authentication::SensitivePassword("bootstrap password"));
    REQUIRE(session);
    auto principal = runtime->sessions()->authenticate(session.value());
    REQUIRE(principal);
    CHECK(sparenode::test::require_optional(principal.value()).identity == "owner");
    auto logout = runtime->session_authentication()->logout(session.value());
    REQUIRE(logout);
    CHECK(sparenode::test::require_optional(logout.value()).identity == "owner");
    auto rejected = runtime->sessions()->authenticate(session.value());
    REQUIRE(rejected);
    CHECK_FALSE(rejected->has_value());
    REQUIRE(runtime->config().servers().size() == 1);
    const auto &server = runtime->config().servers().front();
    CHECK(server.endpoint() == sparenode::network::TcpEndpoint{"127.0.0.1", 8080});
    CHECK_FALSE(server.multithreading_enabled());
    CHECK(server.worker_threads() == 1);
    CHECK(server.minimum_log_severity() == sparenode::logging::LogSeverity::info);
    CHECK(server.http_timeouts().headers == std::chrono::seconds{10});
    CHECK(server.http_timeouts().body == std::chrono::seconds{10});
    CHECK(server.http_timeouts().total == std::chrono::seconds{30});
    CHECK(server.transport_policy().mode == sparenode::http::HttpTransportMode::loopback_http);
    REQUIRE(server.locations().size() == 1);
    CHECK(std::filesystem::equivalent(server.locations().front().root().path(), shared));
    CHECK(server.locations().front().permissions() ==
          sparenode::filesystem::LocationPermissions{true, false, false});
}

TEST_CASE("Runtime bootstrap resolves relative roots from the configuration directory",
          "[bootstrap][configuration][paths]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-bootstrap-relative-root");
    const auto shared = directory.path() / "shared";
    REQUIRE(std::filesystem::create_directory(shared));
    provision_credentials(directory.path() / "credentials.conf");
    const auto config_path = write_file(directory.path() / "spnode.conf",
                                        configuration_source(std::filesystem::path("shared")));

    auto runtime = sparenode::bootstrap::RuntimeBootstrap::build(load_configuration(config_path));

    REQUIRE(runtime.has_value());
    REQUIRE(runtime->config().servers().size() == 1);
    REQUIRE(runtime->config().servers().front().locations().size() == 1);
    CHECK(std::filesystem::equivalent(
        runtime->config().servers().front().locations().front().root().path(), shared));
}

TEST_CASE("Runtime bootstrap resolves external MIME and credential paths",
          "[bootstrap][configuration][paths]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-bootstrap-paths");
    const auto shared = directory.path() / "shared";
    const auto secrets = directory.path() / "secrets";
    REQUIRE(std::filesystem::create_directory(shared));
    REQUIRE(std::filesystem::create_directory(secrets));
    provision_credentials(secrets / "owner.conf");
    static_cast<void>(write_file(directory.path() / "custom.types", "image/png png\n"));
    const auto config_path =
        write_file(directory.path() / "spnode.conf",
                   configuration_source(
                       shared,
                       "bind \"::1\";\nport 8443;\nmultithreading true;\nworker_threads 8;\n"
                       "log_level \"warning\";\nheader_timeout_ms 1500;\nbody_timeout_ms 2500;\n"
                       "request_timeout_ms 90000;\ntransport_mode \"trusted_proxy_https\";\n"
                       "trusted_proxy \"::1\";\nmime_types_file \"custom.types\";\n"
                       "credentials_file \"secrets/owner.conf\";\n",
                       "read false;\nwrite true;\ndelete true;\n"));

    auto runtime = sparenode::bootstrap::RuntimeBootstrap::build(load_configuration(config_path));

    REQUIRE(runtime.has_value());
    const auto &server = runtime->config().servers().front();
    CHECK(server.endpoint() == sparenode::network::TcpEndpoint{"::1", 8443});
    CHECK(server.effective_worker_count() == 8);
    CHECK(server.minimum_log_severity() == sparenode::logging::LogSeverity::warning);
    CHECK(server.http_timeouts().headers == std::chrono::milliseconds{1500});
    CHECK(server.http_timeouts().body == std::chrono::milliseconds{2500});
    CHECK(server.http_timeouts().total == std::chrono::seconds{90});
    CHECK(server.transport_policy().mode ==
          sparenode::http::HttpTransportMode::trusted_proxy_https);
    CHECK(server.transport_policy().trusted_proxy == std::optional<std::string>{"::1"});
    CHECK(server.mime_types().content_type_for_path("file.PNG") == "image/png");
    CHECK(server.credentials_file() == secrets / "owner.conf");
    CHECK(server.locations().front().permissions() ==
          sparenode::filesystem::LocationPermissions{false, true, true});
}

TEST_CASE("Runtime bootstrap retains source locations for external failures",
          "[bootstrap][configuration][errors]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-bootstrap-errors");
    const auto missing_root = directory.path() / "missing";

    SECTION("missing shared root")
    {
        const auto config_path =
            write_file(directory.path() / "spnode.conf", configuration_source(missing_root));
        auto runtime =
            sparenode::bootstrap::RuntimeBootstrap::build(load_configuration(config_path));

        REQUIRE_FALSE(runtime.has_value());
        CHECK(runtime.error().code ==
              sparenode::bootstrap::RuntimeBootstrapErrorCode::invalid_location_root);
        CHECK(runtime.error().source_path == config_path);
        CHECK(runtime.error().location.line == 3);
        REQUIRE(runtime.error().shared_root_error.has_value());
    }

    SECTION("missing MIME file")
    {
        const auto shared = directory.path() / "shared";
        REQUIRE(std::filesystem::create_directory(shared));
        const auto config_path =
            write_file(directory.path() / "spnode.conf",
                       configuration_source(shared, "mime_types_file \"missing.types\";\n"));
        auto runtime =
            sparenode::bootstrap::RuntimeBootstrap::build(load_configuration(config_path));

        REQUIRE_FALSE(runtime.has_value());
        CHECK(runtime.error().code ==
              sparenode::bootstrap::RuntimeBootstrapErrorCode::mime_types_load_failed);
        CHECK(runtime.error().source_path == directory.path() / "missing.types");
        REQUIRE(runtime.error().mime_types_error.has_value());
        CHECK(sparenode::bootstrap::format_runtime_bootstrap_error(runtime.error())
                  .find("missing.types:1:1: error: unable to open MIME types file") !=
              std::string::npos);
    }

    SECTION("missing default credentials retain the server source location")
    {
        const auto shared = directory.path() / "shared";
        REQUIRE(std::filesystem::create_directory(shared));
        const auto config_path =
            write_file(directory.path() / "spnode.conf", configuration_source(shared));
        auto runtime =
            sparenode::bootstrap::RuntimeBootstrap::build(load_configuration(config_path));

        REQUIRE_FALSE(runtime.has_value());
        CHECK(runtime.error().code ==
              sparenode::bootstrap::RuntimeBootstrapErrorCode::authentication_failed);
        CHECK(runtime.error().source_path == directory.path() / "credentials.conf");
        CHECK(runtime.error().location == sparenode::configuration::SourceLocation{0, 1, 1});
        CHECK(runtime.error().authentication_error ==
              sparenode::authentication::FileAuthenticationErrorCode::credential_open_failed);
    }

    SECTION("credentials inside a shared root fail without exposing their contents")
    {
        constexpr std::string_view credential_contents = "credential-secret-that-must-not-appear";
        const auto shared = directory.path() / "shared";
        REQUIRE(std::filesystem::create_directory(shared));
        const auto credential_path = write_file(shared / "owner.conf", credential_contents);
        const auto config_path =
            write_file(directory.path() / "spnode.conf",
                       configuration_source(shared, "credentials_file \"shared/owner.conf\";\n"));
        auto runtime =
            sparenode::bootstrap::RuntimeBootstrap::build(load_configuration(config_path));

        REQUIRE_FALSE(runtime.has_value());
        CHECK(runtime.error().code ==
              sparenode::bootstrap::RuntimeBootstrapErrorCode::authentication_failed);
        CHECK(runtime.error().source_path == credential_path);
        CHECK(runtime.error().authentication_error ==
              sparenode::authentication::FileAuthenticationErrorCode::credential_is_shared);
        CHECK(sparenode::bootstrap::format_runtime_bootstrap_error(runtime.error())
                  .find(credential_contents) == std::string::npos);
    }
}
