#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "sparenode/filesystem/location_permissions.hpp"
#include "sparenode/filesystem/shared_root.hpp"
#include "sparenode/http/api/filesystem_api.hpp"
#include "sparenode/http/transport/http_connection_handler.hpp"
#include "support/connected_tcp_pair.hpp"
#include "support/optional.hpp"
#include "support/temporary_directory.hpp"

namespace
{

[[nodiscard]] std::span<const std::byte> as_bytes(const std::string_view source)
{
    return std::as_bytes(std::span(source.data(), source.size()));
}

[[nodiscard]] sparenode::http::FilesystemApiSettings
make_settings(const sparenode::test::TemporaryDirectory &directory)
{
    auto root = sparenode::filesystem::SharedRoot::create(directory.path());
    REQUIRE(root);
    std::vector<sparenode::http::FilesystemApiLocation> locations;
    locations.push_back(
        {"/api/Documents", sparenode::application::FilesystemService(
                               std::move(root).value(),
                               sparenode::filesystem::LocationPermissions{true, true, false})});
    return {std::move(locations), {}};
}

[[nodiscard]] sparenode::http::HttpApplication
application_for(const sparenode::http::HttpRouter &router)
{
    return [&router](const sparenode::http::HttpRequestView &request)
               -> sparenode::Result<sparenode::http::HttpResponse,
                                    sparenode::http::HttpApplicationError>
    {
        auto response = router.dispatch(request);
        if (!response)
        {
            return sparenode::unexpected(sparenode::http::HttpApplicationError{
                sparenode::http::HttpApplicationErrorCode::request_failed,
                response.error().detail});
        }
        return std::move(response).value();
    };
}

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

[[nodiscard]] std::string read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("Filesystem API publishes fixed and chunked uploads through the streaming session",
          "[http][filesystem-api][upload][integration]")
{
    const bool chunked = GENERATE(false, true);
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-upload");
    auto router = sparenode::http::make_filesystem_api_router(make_settings(directory));
    REQUIRE(router);
    auto pair = sparenode::test::create_connected_tcp_pair();
    std::promise<sparenode::Result<void, sparenode::network::NetworkError>> session_result;
    auto completed = session_result.get_future();
    std::jthread worker(
        [&](const std::stop_token &stop_token)
        {
            session_result.set_value(sparenode::http::handle_http_connection(
                std::move(pair.server), application_for(router.value()), stop_token));
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
    send_all(pair.client, request.str());
    pair.client.shutdown_send();

    const auto response = receive_until_closed(pair.client);
    REQUIRE(completed.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    REQUIRE(completed.get());
    CHECK(response.starts_with("HTTP/1.1 201 Created\r\n"));
    CHECK(response.ends_with("{\"status\":\"created\"}"));
    CHECK(read_file(directory.path() / "upload.bin") == payload);
}

TEST_CASE("Disconnected partial uploads never reach the configured location",
          "[http][filesystem-api][upload][disconnect]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-files-api-upload-disconnect");
    auto router = sparenode::http::make_filesystem_api_router(make_settings(directory));
    REQUIRE(router);
    auto pair = sparenode::test::create_connected_tcp_pair();
    send_all(pair.client, "PUT /api/Documents/partial.txt HTTP/1.1\r\nHost: localhost\r\n"
                          "Content-Length: 10\r\n\r\npartial");
    pair.client.shutdown_send();

    REQUIRE(sparenode::http::handle_http_connection(std::move(pair.server),
                                                    application_for(router.value()), {}));
    const auto response = receive_until_closed(pair.client);
    CHECK(response.starts_with("HTTP/1.1 400 Bad Request\r\n"));
    CHECK_FALSE(std::filesystem::exists(directory.path() / "partial.txt"));
}
