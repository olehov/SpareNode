#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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
#include "sparenode/http/session/http_connection_handler.hpp"
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

/// @brief Returns one exact response field value for assertions.
[[nodiscard]] std::string_view header_value(const sparenode::http::HttpResponse &response,
                                            const std::string_view name)
{
    const auto headers = response.headers();
    const auto header =
        std::ranges::find_if(headers, [name](const sparenode::http::HttpResponseHeader &candidate)
                             { return candidate.name == name; });
    return header == headers.end() ? std::string_view{} : std::string_view(header->value);
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
make_server(const sparenode::test::TemporaryDirectory &directory, const bool allow_read = true,
            sparenode::http::MimeTypeRegistry mime_types = {}, const bool allow_write = false,
            const bool allow_delete = false)
{
    auto root = sparenode::configuration::SharedRoot::create(directory.path());
    REQUIRE(root);
    std::vector<sparenode::configuration::runtime::LocationConfig> locations;
    locations.emplace_back("/api/Documents", std::move(root).value(),
                           sparenode::configuration::runtime::LocationPermissions{
                               allow_read, allow_write, allow_delete});
    return {{"127.0.0.1", 0},
            false,
            1,
            sparenode::logging::LogSeverity::info,
            std::move(locations),
            {},
            std::move(mime_types)};
}

/// @brief Creates one validated MIME registry for response integration tests.
[[nodiscard]] sparenode::http::MimeTypeRegistry
make_mime_types(std::vector<sparenode::http::MimeTypeMapping> mappings)
{
    auto registry = sparenode::http::MimeTypeRegistry::create(std::move(mappings));
    REQUIRE(registry);
    return std::move(registry).value();
}

/// @brief Creates a file symbolic link, skipping only unavailable local Windows privileges.
void create_file_link(const std::filesystem::path &target, const std::filesystem::path &link)
{
    std::error_code error;
    std::filesystem::create_symlink(target, link, error);
#if defined(_WIN32) && !defined(SPARENODE_REQUIRE_SYMLINK_TESTS)
    constexpr int privilege_not_held = 1314; // Win32 ERROR_PRIVILEGE_NOT_HELD.
    if (error == std::error_code(privilege_not_held, std::system_category()))
    {
        SKIP("Windows symbolic-link creation requires Developer Mode or the symlink privilege");
    }
#endif
    INFO(error.message());
    REQUIRE_FALSE(error);
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

/// @brief Dispatches one in-memory PUT request through a validated filesystem router.
[[nodiscard]] sparenode::http::HttpResponse dispatch_put(const sparenode::http::HttpRouter &router,
                                                         const std::string_view target,
                                                         const std::string_view body)
{
    const std::string source =
        "PUT " + std::string(target) +
        " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\n\r\n" + std::string(body);
    auto response = router.dispatch(parse_request(source));
    REQUIRE(response);
    return std::move(response).value();
}

/// @brief Dispatches one POST directory-creation request through a validated filesystem router.
[[nodiscard]] sparenode::http::HttpResponse dispatch_post(const sparenode::http::HttpRouter &router,
                                                          const std::string_view target,
                                                          const std::string_view body = {})
{
    const std::string source =
        "POST " + std::string(target) +
        " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\n\r\n" + std::string(body);
    auto response = router.dispatch(parse_request(source));
    REQUIRE(response);
    return std::move(response).value();
}

/// @brief Dispatches one DELETE request through a validated filesystem router.
[[nodiscard]] sparenode::http::HttpResponse
dispatch_delete(const sparenode::http::HttpRouter &router, const std::string_view target,
                const std::string_view body = {})
{
    const std::string source =
        "DELETE " + std::string(target) +
        " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\n\r\n" + std::string(body);
    auto response = router.dispatch(parse_request(source));
    REQUIRE(response);
    return std::move(response).value();
}

/// @brief Sends all bytes through a potentially partial loopback socket operation.
void send_all(const sparenode::test::TestClientSocket &client, const std::string_view source)
{
    auto remaining = as_bytes(source);
    while (!remaining.empty())
    {
        const auto sent = client.send(remaining);
        REQUIRE(sent > 0);
        remaining = remaining.subspan(static_cast<std::size_t>(sent));
    }
}

/// @brief Receives a response until the server closes its sending direction.
[[nodiscard]] std::string receive_until_closed(const sparenode::test::TestClientSocket &client)
{
    std::string response;
    std::array<std::byte, 512> buffer{};
    while (true)
    {
        const auto received = client.receive_within(buffer, std::chrono::seconds{2});
        const auto count = sparenode::test::require_optional(received);
        REQUIRE(count >= 0);
        if (count == 0)
        {
            return response;
        }
        response.append(reinterpret_cast<const char *>(buffer.data()),
                        static_cast<std::size_t>(count));
    }
}

/// @brief Reads one test file as exact binary text.
[[nodiscard]] std::string read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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

TEST_CASE("Filesystem API publishes fixed and chunked uploads through the streaming session",
          "[http][filesystem-api][upload][integration]")
{
    const bool chunked = GENERATE(false, true);
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-upload");
    auto router =
        sparenode::http::make_filesystem_api_router(make_server(directory, true, {}, true));
    REQUIRE(router);
    auto pair = sparenode::test::create_connected_tcp_pair();
    std::promise<sparenode::Result<void, sparenode::network::NetworkError>> session_result;
    auto completed = session_result.get_future();
    std::jthread worker(
        [&](const std::stop_token &stop_token)
        {
            session_result.set_value(sparenode::http::handle_http_connection(
                std::move(pair.server), router.value(), stop_token));
        });

    std::string payload(std::size_t{32} * 1024, 'x');
    payload.front() = 'a';
    payload.back() = 'z';
    std::ostringstream request;
    request << "PUT /api/Documents/upload.bin HTTP/1.1\r\nHost: localhost\r\n";
    if (chunked)
    {
        request << "Transfer-Encoding: chunked\r\n\r\n"
                << std::hex << payload.size() << "\r\n"
                << payload << "\r\n0\r\n\r\n";
    }
    else
    {
        request << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
    }
    const auto wire_request = request.str();
    send_all(pair.client, wire_request);
    pair.client.shutdown_send();

    const auto response = receive_until_closed(pair.client);
    REQUIRE(completed.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    REQUIRE(completed.get());
    CHECK(response.starts_with("HTTP/1.1 201 Created\r\n"));
    CHECK(response.ends_with("{\"status\":\"created\"}"));
    CHECK(read_file(directory.path() / "upload.bin") == payload);
}

TEST_CASE("Filesystem API enforces write permission before publishing uploads",
          "[http][filesystem-api][upload][permissions]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-upload-permission");
    auto router = sparenode::http::make_filesystem_api_router(make_server(directory));
    REQUIRE(router);

    const auto response = dispatch_put(router.value(), "/api/Documents/rejected.txt", "content");
    CHECK(response.status_code() == sparenode::http::HttpStatusCode::forbidden);
    CHECK(body_text(response) == "{\"error\":\"write_forbidden\"}");
    CHECK_FALSE(std::filesystem::exists(directory.path() / "rejected.txt"));
}

TEST_CASE("Filesystem API never replaces an existing upload destination",
          "[http][filesystem-api][upload][conflict]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-upload-conflict");
    REQUIRE(std::ofstream(directory.path() / "existing.txt", std::ios::binary) << "original");
    auto router =
        sparenode::http::make_filesystem_api_router(make_server(directory, true, {}, true));
    REQUIRE(router);

    const auto response =
        dispatch_put(router.value(), "/api/Documents/existing.txt", "replacement");
    CHECK(response.status_code() == sparenode::http::HttpStatusCode::conflict);
    CHECK(body_text(response) == "{\"error\":\"destination_exists\"}");
    CHECK(read_file(directory.path() / "existing.txt") == "original");
}

TEST_CASE("Filesystem API creates empty uploads and rejects unsafe destinations",
          "[http][filesystem-api][upload][security]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-upload-paths");
    const sparenode::test::TemporaryDirectory outside("sparenode-files-api-upload-outside");
    auto router =
        sparenode::http::make_filesystem_api_router(make_server(directory, true, {}, true));
    REQUIRE(router);

    const auto empty = dispatch_put(router.value(), "/api/Documents/empty.txt", {});
    CHECK(empty.status_code() == sparenode::http::HttpStatusCode::created);
    CHECK(std::filesystem::file_size(directory.path() / "empty.txt") == 0);

    const auto root = dispatch_put(router.value(), "/api/Documents", {});
    CHECK(root.status_code() == sparenode::http::HttpStatusCode::bad_request);
    CHECK(body_text(root) == "{\"error\":\"invalid_path\"}");

    const auto missing_parent =
        dispatch_put(router.value(), "/api/Documents/missing/file.txt", "content");
    CHECK(missing_parent.status_code() == sparenode::http::HttpStatusCode::not_found);
    CHECK(body_text(missing_parent) == "{\"error\":\"parent_not_found\"}");
    CHECK_FALSE(std::filesystem::exists(directory.path() / "missing"));

    const auto outside_name = outside.path().filename().string();
    const auto traversal = dispatch_put(
        router.value(), "/api/Documents/%2E%2E/" + outside_name + "/escaped.txt", "protected");
    CHECK(traversal.status_code() == sparenode::http::HttpStatusCode::not_found);
    CHECK(body_text(traversal) == "{\"error\":\"not_found\"}");
    CHECK_FALSE(std::filesystem::exists(outside.path() / "escaped.txt"));
}

TEST_CASE("Disconnected partial uploads never reach the configured location",
          "[http][filesystem-api][upload][disconnect]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-upload-disconnect");
    auto router =
        sparenode::http::make_filesystem_api_router(make_server(directory, true, {}, true));
    REQUIRE(router);
    auto pair = sparenode::test::create_connected_tcp_pair();
    send_all(pair.client, "PUT /api/Documents/partial.txt HTTP/1.1\r\nHost: localhost\r\n"
                          "Content-Length: 10\r\n\r\npartial");
    pair.client.shutdown_send();

    REQUIRE(sparenode::http::handle_http_connection(std::move(pair.server), router.value(), {}));
    const auto response = receive_until_closed(pair.client);
    CHECK(response.starts_with("HTTP/1.1 400 Bad Request\r\n"));
    CHECK_FALSE(std::filesystem::exists(directory.path() / "partial.txt"));
}

TEST_CASE("Filesystem API creates one directory inside a writable location",
          "[http][filesystem-api][directory-create]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-directory-create");
    REQUIRE(std::filesystem::create_directory(directory.path() / "parent"));
    auto router =
        sparenode::http::make_filesystem_api_router(make_server(directory, true, {}, true));
    REQUIRE(router);

    const auto response = dispatch_post(router.value(), "/api/Documents/parent/new%20directory");

    CHECK(response.status_code() == sparenode::http::HttpStatusCode::created);
    CHECK(body_text(response) == "{\"status\":\"created\"}");
    CHECK(std::filesystem::is_directory(directory.path() / "parent" / "new directory"));
}

TEST_CASE("Filesystem API enforces write permission for directory creation",
          "[http][filesystem-api][directory-create][permissions]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-directory-denied");
    auto router = sparenode::http::make_filesystem_api_router(make_server(directory));
    REQUIRE(router);

    const auto response = dispatch_post(router.value(), "/api/Documents/rejected");

    CHECK(response.status_code() == sparenode::http::HttpStatusCode::forbidden);
    CHECK(body_text(response) == "{\"error\":\"write_forbidden\"}");
    CHECK_FALSE(std::filesystem::exists(directory.path() / "rejected"));
}

TEST_CASE("Filesystem API reports directory conflicts and request errors",
          "[http][filesystem-api][directory-create][errors]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-directory-errors");
    REQUIRE(std::filesystem::create_directory(directory.path() / "existing"));
    auto router =
        sparenode::http::make_filesystem_api_router(make_server(directory, true, {}, true));
    REQUIRE(router);

    const auto conflict = dispatch_post(router.value(), "/api/Documents/existing");
    CHECK(conflict.status_code() == sparenode::http::HttpStatusCode::conflict);
    CHECK(body_text(conflict) == "{\"error\":\"destination_exists\"}");

    const auto missing_parent = dispatch_post(router.value(), "/api/Documents/missing/child");
    CHECK(missing_parent.status_code() == sparenode::http::HttpStatusCode::not_found);
    CHECK(body_text(missing_parent) == "{\"error\":\"parent_not_found\"}");

    const auto root = dispatch_post(router.value(), "/api/Documents");
    CHECK(root.status_code() == sparenode::http::HttpStatusCode::bad_request);
    CHECK(body_text(root) == "{\"error\":\"invalid_path\"}");

    const auto body = dispatch_post(router.value(), "/api/Documents/with-body", "unexpected");
    CHECK(body.status_code() == sparenode::http::HttpStatusCode::bad_request);
    CHECK(body_text(body) == "{\"error\":\"body_not_allowed\"}");
    CHECK_FALSE(std::filesystem::exists(directory.path() / "with-body"));
}

TEST_CASE("Filesystem API confines directory creation to the configured root",
          "[http][filesystem-api][directory-create][security]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-directory-shared");
    const sparenode::test::TemporaryDirectory outside("sparenode-files-api-directory-outside");
    auto router =
        sparenode::http::make_filesystem_api_router(make_server(directory, true, {}, true));
    REQUIRE(router);

    const auto target = "/api/Documents/%2E%2E/" + outside.path().filename().string() + "/escaped";
    const auto response = dispatch_post(router.value(), target);

    CHECK(response.status_code() == sparenode::http::HttpStatusCode::not_found);
    CHECK(body_text(response) == "{\"error\":\"not_found\"}");
    CHECK_FALSE(std::filesystem::exists(outside.path() / "escaped"));
}

TEST_CASE("Filesystem API keeps deletion disabled by default",
          "[http][filesystem-api][delete][permissions]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-delete-denied");
    REQUIRE(std::ofstream(directory.path() / "retained.txt") << "content");
    auto router = sparenode::http::make_filesystem_api_router(make_server(directory));
    REQUIRE(router);

    const auto response = dispatch_delete(router.value(), "/api/Documents/retained.txt");

    CHECK(response.status_code() == sparenode::http::HttpStatusCode::forbidden);
    CHECK(body_text(response) == "{\"error\":\"delete_forbidden\"}");
    CHECK(std::filesystem::exists(directory.path() / "retained.txt"));
}

TEST_CASE("Filesystem API deletes files and empty directories when explicitly enabled",
          "[http][filesystem-api][delete]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-delete");
    REQUIRE(std::ofstream(directory.path() / "file.txt") << "content");
    REQUIRE(std::filesystem::create_directory(directory.path() / "empty"));
    auto router =
        sparenode::http::make_filesystem_api_router(make_server(directory, true, {}, false, true));
    REQUIRE(router);

    const auto file = dispatch_delete(router.value(), "/api/Documents/file.txt");
    const auto empty = dispatch_delete(router.value(), "/api/Documents/empty");

    CHECK(file.status_code() == sparenode::http::HttpStatusCode::ok);
    CHECK(body_text(file) == "{\"status\":\"deleted\"}");
    CHECK(empty.status_code() == sparenode::http::HttpStatusCode::ok);
    CHECK(!std::filesystem::exists(directory.path() / "file.txt"));
    CHECK(!std::filesystem::exists(directory.path() / "empty"));
}

TEST_CASE("Filesystem API reports safe deletion failures",
          "[http][filesystem-api][delete][errors][security]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-delete-errors");
    const sparenode::test::TemporaryDirectory outside("sparenode-files-api-delete-outside");
    const auto populated = directory.path() / "populated";
    REQUIRE(std::filesystem::create_directory(populated));
    REQUIRE(std::ofstream(populated / "child.txt") << "content");
    REQUIRE(std::ofstream(outside.path() / "retained.txt") << "content");
    auto router =
        sparenode::http::make_filesystem_api_router(make_server(directory, true, {}, false, true));
    REQUIRE(router);

    const auto root = dispatch_delete(router.value(), "/api/Documents");
    CHECK(root.status_code() == sparenode::http::HttpStatusCode::bad_request);
    const auto nonempty = dispatch_delete(router.value(), "/api/Documents/populated");
    CHECK(nonempty.status_code() == sparenode::http::HttpStatusCode::conflict);
    CHECK(body_text(nonempty) == "{\"error\":\"directory_not_empty\"}");
    const auto missing = dispatch_delete(router.value(), "/api/Documents/missing");
    CHECK(missing.status_code() == sparenode::http::HttpStatusCode::not_found);
    const auto body = dispatch_delete(router.value(), "/api/Documents/populated", "unexpected");
    CHECK(body.status_code() == sparenode::http::HttpStatusCode::bad_request);
    CHECK(body_text(body) == "{\"error\":\"body_not_allowed\"}");
    CHECK(std::filesystem::exists(populated / "child.txt"));

    const auto traversal_target =
        "/api/Documents/%2E%2E/" + outside.path().filename().string() + "/retained.txt";
    const auto traversal = dispatch_delete(router.value(), traversal_target);
    CHECK(traversal.status_code() == sparenode::http::HttpStatusCode::not_found);
    CHECK(body_text(traversal) == "{\"error\":\"not_found\"}");
    CHECK(std::filesystem::exists(outside.path() / "retained.txt"));
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
    REQUIRE(file.headers().size() == 3);
    CHECK(header_value(file, "Content-Type") == "application/octet-stream");
    CHECK(header_value(file, "Content-Disposition") == "inline");
    CHECK(header_value(file, "X-Content-Type-Options") == "nosniff");
    const std::string expected = sparenode::http::serialize_http_response_head(file) + "download";
    auto pair = sparenode::test::create_connected_tcp_pair();
    REQUIRE(sparenode::http::write_http_response(pair.server, file));
    CHECK(receive_exact(pair.client, expected.size()) == expected);

    const auto traversal = dispatch_get(router.value(), "/api/Documents/../outside");
    CHECK(traversal.status_code() == sparenode::http::HttpStatusCode::not_found);
    CHECK(body_text(traversal) == "{\"error\":\"not_found\"}");
}

TEST_CASE("Filesystem API selects conservative inline content types",
          "[http][filesystem-api][inline][content-type]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-content-type");
    const std::array files{
        std::pair{"notes.TXT", "plain"},   std::pair{"data.json", "{}"},
        std::pair{"image.JpG", "jpeg"},    std::pair{"page.html", "markup"},
        std::pair{"source.cpp", "source"}, std::pair{"payload.custom", "binary"}};
    for (const auto &[name, content] : files)
    {
        std::ofstream output(directory.path() / name, std::ios::binary);
        output << content;
        REQUIRE(output.good());
    }
    auto router = sparenode::http::make_filesystem_api_router(
        make_server(directory, true,
                    make_mime_types({{"txt", "text/plain"},
                                     {"json", "application/json"},
                                     {"jpg", "image/jpeg"},
                                     {"html", "text/html"},
                                     {"cpp", "text/plain"}})));
    REQUIRE(router);

    const auto text = dispatch_get(router.value(), "/api/Documents/notes.TXT");
    CHECK(header_value(text, "Content-Type") == "text/plain");
    CHECK(header_value(text, "Content-Disposition") == "inline");
    CHECK(header_value(text, "X-Content-Type-Options") == "nosniff");

    const auto json = dispatch_get(router.value(), "/api/Documents/data%2Ejson");
    CHECK(header_value(json, "Content-Type") == "application/json");
    CHECK(header_value(dispatch_get(router.value(), "/api/Documents/image.JpG"), "Content-Type") ==
          "image/jpeg");
    CHECK(header_value(dispatch_get(router.value(), "/api/Documents/page.html"), "Content-Type") ==
          "text/plain; charset=utf-8");
    CHECK(header_value(dispatch_get(router.value(), "/api/Documents/source.cpp"), "Content-Type") ==
          "text/plain");
    CHECK(header_value(dispatch_get(router.value(), "/api/Documents/payload.custom"),
                       "Content-Type") == "application/octet-stream");
}

TEST_CASE("Filesystem API preserves inline file metadata for HEAD without consuming content",
          "[http][filesystem-api][inline][head]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-head");
    constexpr std::string_view payload = "preview";
    {
        std::ofstream output(directory.path() / "preview.md", std::ios::binary);
        output << payload;
        REQUIRE(output.good());
    }
    auto router = sparenode::http::make_filesystem_api_router(
        make_server(directory, true, make_mime_types({{"md", "text/markdown"}})));
    REQUIRE(router);
    constexpr std::string_view target = "/api/Documents/preview.md";

    const auto get = dispatch_get(router.value(), target);
    const std::string request =
        "HEAD " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto head_result = router->dispatch(parse_request(request));
    REQUIRE(head_result);
    auto head = std::move(head_result).value();
    CHECK(head.content_length() == get.content_length());
    CHECK(head.headers().size() == get.headers().size());
    CHECK(header_value(head, "Content-Type") == "text/markdown");

    const auto serialized_head = sparenode::http::serialize_http_response_head(head);
    auto head_pair = sparenode::test::create_connected_tcp_pair();
    REQUIRE(sparenode::http::write_http_response(head_pair.server, head, {},
                                                 sparenode::http::HttpMethod::head));
    CHECK(receive_exact(head_pair.client, serialized_head.size()) == serialized_head);

    const std::string complete_response = serialized_head + std::string(payload);
    auto get_pair = sparenode::test::create_connected_tcp_pair();
    REQUIRE(sparenode::http::write_http_response(get_pair.server, head));
    CHECK(receive_exact(get_pair.client, complete_response.size()) == complete_response);
}

TEST_CASE("Filesystem API keeps large inline files outside response memory",
          "[http][filesystem-api][inline][large]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-large-inline");
    constexpr std::uint64_t file_size =
        static_cast<std::uint64_t>(sparenode::http::HttpResponse::maximum_memory_body_bytes) + 1;
    const auto path = directory.path() / "large.bin";
    {
        std::ofstream output(path, std::ios::binary);
        output.seekp(static_cast<std::streamoff>(file_size - 1));
        output.put('\0');
        REQUIRE(output.good());
    }
    auto router = sparenode::http::make_filesystem_api_router(make_server(directory));
    REQUIRE(router);

    const auto response = dispatch_get(router.value(), "/api/Documents/large.bin");
    CHECK(response.is_streaming());
    CHECK(response.memory_body().empty());
    CHECK(response.content_length() == file_size);
    CHECK(header_value(response, "Content-Type") == "application/octet-stream");
}

TEST_CASE("Filesystem API confines inline symbolic-link targets to the configured root",
          "[http][filesystem-api][inline][security][symlink]")
{
    const sparenode::test::TemporaryDirectory shared("sparenode-files-api-inline-shared");
    const sparenode::test::TemporaryDirectory outside("sparenode-files-api-inline-outside");
    const auto inside_file = shared.path() / "inside.txt";
    const auto outside_file = outside.path() / "secret.txt";
    REQUIRE(std::ofstream(inside_file, std::ios::binary) << "allowed");
    REQUIRE(std::ofstream(outside_file, std::ios::binary) << "protected");
    create_file_link(inside_file, shared.path() / "internal.txt");
    create_file_link(outside_file, shared.path() / "external.txt");
    auto router = sparenode::http::make_filesystem_api_router(make_server(shared));
    REQUIRE(router);

    const auto internal = dispatch_get(router.value(), "/api/Documents/internal.txt");
    CHECK(internal.status_code() == sparenode::http::HttpStatusCode::ok);
    CHECK(internal.is_streaming());
    CHECK(internal.content_length() == 7);

    const auto external = dispatch_get(router.value(), "/api/Documents/external.txt");
    CHECK(external.status_code() == sparenode::http::HttpStatusCode::not_found);
    CHECK(body_text(external) == "{\"error\":\"not_found\"}");
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
