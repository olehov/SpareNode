#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/configuration/runtime/location_config.hpp"
#include "sparenode/configuration/runtime/location_permissions.hpp"
#include "sparenode/configuration/runtime/server_config.hpp"
#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/http/api/filesystem_api.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/http/request/http_request_parser.hpp"
#include "sparenode/http/response/http_response_writer.hpp"
#include "sparenode/logging/log_severity.hpp"
#include "sparenode/network/tcp_endpoint.hpp"
#include "support/connected_tcp_pair.hpp"
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

/// @brief Receives one exact response size from a loopback test client.
[[nodiscard]] std::string receive_exact(const sparenode::test::TestClientSocket &client,
                                        const std::size_t size)
{
    std::string result(size, '\0');
    std::size_t received = 0;
    while (received < result.size())
    {
        auto destination =
            std::as_writable_bytes(std::span(result.data() + received, result.size() - received));
        const auto count = client.receive(destination);
        REQUIRE(count > 0);
        received += static_cast<std::size_t>(count);
    }
    return result;
}

/// @brief Creates one server backed by the supplied temporary location.
[[nodiscard]] sparenode::configuration::runtime::ServerConfig
make_server(const sparenode::test::TemporaryDirectory &directory, const bool allow_read = true)
{
    auto root = sparenode::configuration::SharedRoot::create(directory.path());
    REQUIRE(root);
    std::vector<sparenode::configuration::runtime::LocationConfig> locations;
    locations.emplace_back(
        "/api/Documents", std::move(root).value(),
        sparenode::configuration::runtime::LocationPermissions{allow_read, false, false});
    return {
        {"127.0.0.1", 0}, false, 1, sparenode::logging::LogSeverity::info, std::move(locations)};
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

    const auto root = dispatch_get(router.value(), "/api/Documents");
    CHECK(root.status_code() == sparenode::http::HttpStatusCode::ok);
    REQUIRE(root.headers().size() == 1);
    CHECK(root.headers().front().name == "Content-Type");
    CHECK(root.headers().front().value == "application/json; charset=utf-8");
    const auto root_body = body_text(root);
    CHECK(root_body.find("\"share\"") == std::string::npos);
    CHECK(root_body.find("/api/Documents") == std::string::npos);
    CHECK(root_body.find("\"name\":\"nested folder\"") != std::string::npos);
    CHECK(root_body.find("\"name\":\"report.txt\"") != std::string::npos);
    CHECK(root_body.find("\"type\":\"file\"") != std::string::npos);
    CHECK(root_body.find("\"size\":3") != std::string::npos);
    CHECK(root_body.find("\"modifiedAt\":\"") != std::string::npos);
    const auto native_root = directory.path().u8string();
    CHECK(root_body.find(std::string(native_root.begin(), native_root.end())) == std::string::npos);

    const auto child = dispatch_get(router.value(), "/api/Documents/nested%20folder");
    CHECK(child.status_code() == sparenode::http::HttpStatusCode::ok);
    CHECK(body_text(child).find("inside.txt") != std::string::npos);
}

TEST_CASE("Filesystem API enforces read permission", "[http][filesystem-api][permissions]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-permission");
    auto router = sparenode::http::make_filesystem_api_router(make_server(directory, false));
    REQUIRE(router);

    const auto response = dispatch_get(router.value(), "/api/Documents");
    CHECK(response.status_code() == sparenode::http::HttpStatusCode::forbidden);
    CHECK(body_text(response) == "{\"error\":\"read_forbidden\"}");
}

TEST_CASE("Filesystem API registers multiple configured locations with segment boundaries",
          "[http][filesystem-api][locations]")
{
    const sparenode::test::TemporaryDirectory documents("sparenode-files-documents");
    const sparenode::test::TemporaryDirectory media("sparenode-files-media");
    REQUIRE(std::ofstream(documents.path() / "document.txt").good());
    REQUIRE(std::ofstream(media.path() / "photo.jpg").good());
    auto documents_root = sparenode::configuration::SharedRoot::create(documents.path());
    auto media_root = sparenode::configuration::SharedRoot::create(media.path());
    REQUIRE(documents_root);
    REQUIRE(media_root);

    std::vector<sparenode::configuration::runtime::LocationConfig> locations;
    locations.emplace_back("/api/docs", std::move(documents_root).value(),
                           sparenode::configuration::runtime::LocationPermissions{});
    locations.emplace_back("/api/media", std::move(media_root).value(),
                           sparenode::configuration::runtime::LocationPermissions{});
    const sparenode::configuration::runtime::ServerConfig server(
        {"127.0.0.1", 0}, false, 1, sparenode::logging::LogSeverity::info, std::move(locations));
    auto router = sparenode::http::make_filesystem_api_router(server);
    REQUIRE(router);

    CHECK(body_text(dispatch_get(router.value(), "/api/docs")).find("document.txt") !=
          std::string::npos);
    CHECK(body_text(dispatch_get(router.value(), "/api/media")).find("photo.jpg") !=
          std::string::npos);
    CHECK(dispatch_get(router.value(), "/api/docs-old").status_code() ==
          sparenode::http::HttpStatusCode::not_found);
}

TEST_CASE("Filesystem API maps paths to safe public errors", "[http][filesystem-api][errors]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-errors");
    {
        std::ofstream file_output(directory.path() / "file.txt", std::ios::binary);
        file_output << "download";
        REQUIRE(file_output.good());
    }
    auto router = sparenode::http::make_filesystem_api_router(make_server(directory));
    REQUIRE(router);

    const auto missing = dispatch_get(router.value(), "/api/Documents/missing");
    CHECK(missing.status_code() == sparenode::http::HttpStatusCode::not_found);
    CHECK(body_text(missing) == "{\"error\":\"not_found\"}");

    auto file = dispatch_get(router.value(), "/api/Documents/file.txt");
    CHECK(file.status_code() == sparenode::http::HttpStatusCode::ok);
    CHECK(file.is_streaming());
    CHECK(file.content_length() == 8);
    REQUIRE(file.headers().size() == 1);
    CHECK(file.headers().front().name == "Content-Type");
    CHECK(file.headers().front().value == "application/octet-stream");
    const std::string expected = sparenode::http::serialize_http_response_head(file) + "download";
    auto pair = sparenode::test::create_connected_tcp_pair();
    REQUIRE(sparenode::http::write_http_response(pair.server, file));
    CHECK(receive_exact(pair.client, expected.size()) == expected);

    const auto traversal = dispatch_get(router.value(), "/api/Documents/../outside");
    CHECK(traversal.status_code() == sparenode::http::HttpStatusCode::not_found);
    CHECK(body_text(traversal) == "{\"error\":\"not_found\"}");
}

TEST_CASE("Filesystem API rejects runtime configuration without a location",
          "[http][filesystem-api][startup]")
{
    const sparenode::configuration::runtime::ServerConfig server(
        {"127.0.0.1", 0}, false, 1, sparenode::logging::LogSeverity::info, {});
    const auto router = sparenode::http::make_filesystem_api_router(server);
    REQUIRE_FALSE(router);
    CHECK(router.error().code == sparenode::http::FilesystemApiErrorCode::missing_location);
}
