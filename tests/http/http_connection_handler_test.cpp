#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "sparenode/http/http_connection_handler.hpp"
#include "sparenode/http/http_response.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/network/connection_server.hpp"
#include "support/connected_tcp_pair.hpp"
#include "support/optional.hpp"

namespace
{

using SessionResult = sparenode::Result<void, sparenode::network::NetworkError>;

/// @brief Borrows string storage as bytes for loopback transmission.
/// @param[in] text Request fixture to expose.
/// @return Immutable byte view over the same storage.
[[nodiscard]] std::span<const std::byte> bytes_of(const std::string_view text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

/// @brief Sends every request byte through a potentially partial native socket operation.
/// @param[in] client Connected native test client.
/// @param[in] text Request bytes to send.
void send_all(const sparenode::test::TestClientSocket &client, const std::string_view text)
{
    auto remaining = bytes_of(text);
    while (!remaining.empty())
    {
        const std::ptrdiff_t sent = client.send(remaining);
        REQUIRE(sent > 0);
        remaining = remaining.subspan(static_cast<std::size_t>(sent));
    }
}

/// @brief Receives response bytes until the session closes its side of the connection.
/// @param[in] client Connected native test client.
/// @return Complete response bytes observed before orderly shutdown.
[[nodiscard]] std::string receive_until_closed(const sparenode::test::TestClientSocket &client)
{
    std::string response;
    std::array<std::byte, 512> buffer{};
    while (true)
    {
        const auto received = client.receive_within(buffer, std::chrono::seconds{1});
        const std::ptrdiff_t received_bytes = sparenode::test::require_optional(received);
        REQUIRE(received_bytes >= 0);
        if (received_bytes == 0)
        {
            return response;
        }
        response.append(reinterpret_cast<const char *>(buffer.data()),
                        static_cast<std::size_t>(received_bytes));
    }
}

/// @brief Creates one fixed successful route response for session tests.
/// @return Valid empty HTTP response.
[[nodiscard]] sparenode::Result<sparenode::http::HttpResponse, sparenode::http::HttpRouteError>
ok_response()
{
    auto response =
        sparenode::http::HttpResponse::create(sparenode::http::HttpStatusCode::ok, "OK", {}, {});
    if (!response)
    {
        return sparenode::unexpected(sparenode::http::HttpRouteError{
            sparenode::http::HttpRouteErrorCode::response_validation_failure, 0});
    }
    return std::move(response).value();
}

} // namespace

TEST_CASE("HTTP connection session reads incrementally routes and writes a response",
          "[http][session][integration]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    REQUIRE(router.register_route(
        sparenode::http::HttpMethod::get, "/api/status",
        [](const sparenode::http::HttpRequestView &, const sparenode::http::HttpRouteParameters &)
        { return ok_response(); }));
    std::promise<SessionResult> promise;
    auto future = promise.get_future();
    std::jthread worker(
        [&]
        {
            promise.set_value(sparenode::http::handle_http_connection(
                std::move(pair.server), router, {},
                {.receive_chunk_bytes = 7, .deadline_provider = {}}));
        });

    send_all(pair.client, "GET /api/status HTTP/1.1\r\nHost: local");
    send_all(pair.client, "host\r\n\r\n");
    const std::string response = receive_until_closed(pair.client);
    const auto result = future.get();
    worker.join();

    REQUIRE(result.has_value());
    CHECK(response == "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
}

TEST_CASE("HTTP connection session returns a bounded parser error response",
          "[http][session][integration][error]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    std::promise<SessionResult> promise;
    auto future = promise.get_future();
    std::jthread worker(
        [&]
        {
            promise.set_value(
                sparenode::http::handle_http_connection(std::move(pair.server), router, {}));
        });

    send_all(pair.client, "GET /%GG HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const std::string response = receive_until_closed(pair.client);
    const auto result = future.get();
    worker.join();

    REQUIRE(result.has_value());
    CHECK(response.starts_with("HTTP/1.1 400 Bad Request\r\n"));
    CHECK(response.contains("Connection: close\r\n"));
}

TEST_CASE("HTTP connection session handles one request and discards pipelined bytes on close",
          "[http][session][integration][pipeline]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    REQUIRE(router.register_route(
        sparenode::http::HttpMethod::get, "/one",
        [](const sparenode::http::HttpRequestView &, const sparenode::http::HttpRouteParameters &)
        { return ok_response(); }));
    std::promise<SessionResult> promise;
    auto future = promise.get_future();
    std::jthread worker(
        [&]
        {
            promise.set_value(
                sparenode::http::handle_http_connection(std::move(pair.server), router, {}));
        });

    send_all(pair.client, "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n"
                          "GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const std::string response = receive_until_closed(pair.client);
    const auto result = future.get();
    worker.join();

    REQUIRE(result.has_value());
    CHECK(response == "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    CHECK(response.find("HTTP/1.1", 1) == std::string::npos);
}

TEST_CASE("HTTP connection session applies its default deadline policy",
          "[http][session][integration][timeout]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    const sparenode::http::HttpConnectionHandlerConfig config{
        .request_timeout = std::chrono::milliseconds{20},
        .deadline_provider = {},
    };

    const auto result =
        sparenode::http::handle_http_connection(std::move(pair.server), router, {}, config);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().operation == sparenode::network::NetworkOperation::receive);
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::timeout);
    CHECK(pair.client.peer_closes_within(std::chrono::seconds{1}));
}

TEST_CASE("HTTP connection session injects deadline policy into request reads",
          "[http][session][integration][timeout]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    const sparenode::http::HttpConnectionHandlerConfig config{
        .deadline_provider =
            [](const sparenode::http::HttpRequestReadPhase,
               const sparenode::network::NetworkDeadline)
        {
            return std::optional<sparenode::network::NetworkDeadline>{
                std::chrono::steady_clock::now() + std::chrono::milliseconds{20}};
        },
    };

    const auto result =
        sparenode::http::handle_http_connection(std::move(pair.server), router, {}, config);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().operation == sparenode::network::NetworkOperation::receive);
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::timeout);
    CHECK(pair.client.peer_closes_within(std::chrono::seconds{1}));
}

TEST_CASE("HTTP connection session distinguishes header and body read phases",
          "[http][session][integration][body]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    std::size_t observed_body_size = 0;
    REQUIRE(
        router.register_route(sparenode::http::HttpMethod::post, "/upload",
                              [&observed_body_size](const sparenode::http::HttpRequestView &request,
                                                    const sparenode::http::HttpRouteParameters &)
                              {
                                  observed_body_size = request.body().size();
                                  return ok_response();
                              }));
    std::vector<sparenode::http::HttpRequestReadPhase> phases;
    constexpr std::string_view headers =
        "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n";
    const sparenode::http::HttpConnectionHandlerConfig config{
        .receive_chunk_bytes = headers.size(),
        .deadline_provider =
            [&phases](const sparenode::http::HttpRequestReadPhase phase,
                      const sparenode::network::NetworkDeadline)
        {
            phases.push_back(phase);
            return std::optional<sparenode::network::NetworkDeadline>{};
        },
    };
    std::promise<SessionResult> promise;
    auto future = promise.get_future();
    std::jthread worker(
        [&]
        {
            promise.set_value(sparenode::http::handle_http_connection(std::move(pair.server),
                                                                      router, {}, config));
        });

    send_all(pair.client, headers);
    send_all(pair.client, "data");
    static_cast<void>(receive_until_closed(pair.client));
    const auto result = future.get();
    worker.join();

    REQUIRE(result.has_value());
    REQUIRE(phases.size() >= 2);
    CHECK(phases.front() == sparenode::http::HttpRequestReadPhase::headers);
    CHECK(phases.back() == sparenode::http::HttpRequestReadPhase::body);
    CHECK(observed_body_size == 4);
}

TEST_CASE("HTTP connection session rejects an invalid receive boundary", "[http][session][limits]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;

    const auto result = sparenode::http::handle_http_connection(
        std::move(pair.server), router, {}, {.receive_chunk_bytes = 0, .deadline_provider = {}});

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::validation);
}

TEST_CASE("HTTP connection handler runs through the TCP dispatcher",
          "[http][session][integration][dispatcher]")
{
    auto router = std::make_shared<sparenode::http::HttpRouter>();
    REQUIRE(router->register_route(
        sparenode::http::HttpMethod::get, "/health",
        [](const sparenode::http::HttpRequestView &, const sparenode::http::HttpRouteParameters &)
        { return ok_response(); }));
    auto handler = sparenode::http::make_http_connection_handler(router);
    auto server_result = sparenode::network::ConnectionServer::start(
        {{"127.0.0.1", 0}, 128, false, {{1, 4}, std::move(handler), {}}, {}});
    REQUIRE(server_result.has_value());
    auto server = std::move(server_result).value();
    const auto local_endpoint = server.local_endpoint();
    const auto &endpoint = sparenode::test::require_optional(local_endpoint);
    auto client = sparenode::test::connect_test_client(endpoint);

    send_all(client, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const std::string response = receive_until_closed(client);
    server.request_stop();

    CHECK(response == "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
}

TEST_CASE("HTTP connection session observes dispatcher cancellation",
          "[http][session][integration][cancel]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    std::stop_source stop_source;
    stop_source.request_stop();

    const auto result = sparenode::http::handle_http_connection(std::move(pair.server), router,
                                                                stop_source.get_token());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().operation == sparenode::network::NetworkOperation::receive);
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::cancellation);
    CHECK(pair.client.peer_closes_within(std::chrono::seconds{1}));
}
