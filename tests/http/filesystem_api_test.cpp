#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/configuration/runtime/server_config.hpp"
#include "sparenode/configuration/runtime/share_config.hpp"
#include "sparenode/configuration/runtime/share_permissions.hpp"
#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/http/filesystem_api.hpp"
#include "sparenode/http/http_request_parser.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/logging/log_severity.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "support/optional.hpp"
#include "support/temporary_directory.hpp"

namespace
{

/// @brief Exposes immutable request text as bytes.
[[nodiscard]] std::span<const std::byte> as_bytes(const std::string_view source)
{
    return std::as_bytes(std::span(source.data(), source.size()));
}

/// @brief Parses one complete request whose source storage remains alive.
[[nodiscard]] sparenode::http::HttpRequestView parse_request(const std::string_view source)
{
    auto parsed = sparenode::http::parse_http_request(as_bytes(source));
    REQUIRE(parsed);
    REQUIRE(parsed->is_complete());
    return sparenode::test::require_optional(parsed->request());
}

/// @brief Copies a memory response body into text for assertions.
[[nodiscard]] std::string body_text(const sparenode::http::HttpResponse &response)
{
    const auto body = response.memory_body();
    return {reinterpret_cast<const char *>(body.data()), body.size()};
}

/// @brief Creates one server backed by the supplied temporary share.
[[nodiscard]] sparenode::configuration::runtime::ServerConfig
make_server(const sparenode::test::TemporaryDirectory &directory, const bool allow_read = true)
{
    auto root = sparenode::configuration::SharedRoot::create(directory.path());
    REQUIRE(root);
    std::vector<sparenode::configuration::runtime::ShareConfig> shares;
    shares.emplace_back(
        "Doc\"uments", std::move(root).value(),
        sparenode::configuration::runtime::SharePermissions{allow_read, false, false});
    return {{"127.0.0.1", 0}, false, 1, sparenode::logging::LogSeverity::info, std::move(shares)};
}

/// @brief Dispatches one GET request through a validated filesystem router.
[[nodiscard]] sparenode::http::HttpResponse dispatch_get(const sparenode::http::HttpRouter &router,
                                                         const std::string_view target)
{
    const std::string source =
        "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto response = router.dispatch(parse_request(source));
    REQUIRE(response);
    return std::move(response).value();
}

} // namespace

TEST_CASE("Filesystem API lists root and nested directories as JSON", "[http][filesystem-api]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api");
    const auto nested = directory.path() / "nested folder";
    std::filesystem::create_directory(nested);
    {
        std::ofstream file(directory.path() / "report.txt", std::ios::binary);
        file << "abc";
    }
    REQUIRE(std::ofstream(nested / "inside.txt").good());
    auto router = sparenode::http::make_filesystem_api_router(make_server(directory));
    REQUIRE(router);

    const auto root = dispatch_get(router.value(), "/api/files");
    CHECK(root.status_code() == sparenode::http::HttpStatusCode::ok);
    REQUIRE(root.headers().size() == 1);
    CHECK(root.headers().front().name == "Content-Type");
    CHECK(root.headers().front().value == "application/json; charset=utf-8");
    const auto root_body = body_text(root);
    CHECK(root_body.find("\"share\":\"Doc\\\"uments\"") != std::string::npos);
    CHECK(root_body.find("\"name\":\"nested folder\"") != std::string::npos);
    CHECK(root_body.find("\"name\":\"report.txt\"") != std::string::npos);
    CHECK(root_body.find("\"type\":\"file\"") != std::string::npos);
    CHECK(root_body.find("\"size\":3") != std::string::npos);
    CHECK(root_body.find("\"modifiedAt\":\"") != std::string::npos);
    const auto native_root = directory.path().u8string();
    CHECK(root_body.find(std::string(native_root.begin(), native_root.end())) == std::string::npos);

    const auto child = dispatch_get(router.value(), "/api/files/nested%20folder");
    CHECK(child.status_code() == sparenode::http::HttpStatusCode::ok);
    CHECK(body_text(child).find("inside.txt") != std::string::npos);
}

TEST_CASE("Filesystem API enforces read permission", "[http][filesystem-api][permissions]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-permission");
    auto router = sparenode::http::make_filesystem_api_router(make_server(directory, false));
    REQUIRE(router);

    const auto response = dispatch_get(router.value(), "/api/files");
    CHECK(response.status_code() == sparenode::http::HttpStatusCode::forbidden);
    CHECK(body_text(response) == "{\"error\":\"read_forbidden\"}");
}

TEST_CASE("Filesystem API maps paths to safe public errors", "[http][filesystem-api][errors]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-errors");
    REQUIRE(std::ofstream(directory.path() / "file.txt").good());
    auto router = sparenode::http::make_filesystem_api_router(make_server(directory));
    REQUIRE(router);

    const auto missing = dispatch_get(router.value(), "/api/files/missing");
    CHECK(missing.status_code() == sparenode::http::HttpStatusCode::not_found);
    CHECK(body_text(missing) == "{\"error\":\"not_found\"}");

    const auto file = dispatch_get(router.value(), "/api/files/file.txt");
    CHECK(file.status_code() == sparenode::http::HttpStatusCode::bad_request);
    CHECK(body_text(file) == "{\"error\":\"not_directory\"}");

    const auto traversal = dispatch_get(router.value(), "/api/files/../outside");
    CHECK(traversal.status_code() == sparenode::http::HttpStatusCode::not_found);
    CHECK(body_text(traversal) == "{\"error\":\"not_found\"}");
}

TEST_CASE("Filesystem API rejects runtime configuration without a share",
          "[http][filesystem-api][startup]")
{
    const sparenode::configuration::runtime::ServerConfig server(
        {"127.0.0.1", 0}, false, 1, sparenode::logging::LogSeverity::info, {});
    const auto router = sparenode::http::make_filesystem_api_router(server);
    REQUIRE_FALSE(router);
    CHECK(router.error().code == sparenode::http::FilesystemApiErrorCode::missing_share);
}
