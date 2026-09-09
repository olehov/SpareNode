#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <optional>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "sparenode/http/http_connection_handler.hpp"
#include "sparenode/http/http_response.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/logging/console_log_sink.hpp"
#include "sparenode/logging/network_logging.hpp"
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
    CHECK(response == "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
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
    CHECK(response == "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    CHECK(response.find("HTTP/1.1", 1) == std::string::npos);
}

TEST_CASE("HTTP connection session applies its default deadline policy",
          "[http][session][integration][timeout]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    const sparenode::http::HttpConnectionHandlerConfig config{
        .timeouts = {.total = std::chrono::milliseconds{20}},
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

TEST_CASE("HTTP connection session accepts a timeout near the deadline representation limit",
          "[http][session][limits][timeout]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    const auto remaining = (sparenode::network::NetworkDeadline::max)().time_since_epoch() -
                           std::chrono::steady_clock::now().time_since_epoch();
    // Allow for scheduler delays before the handler takes its own clock sample.
    constexpr auto clock_sample_margin = std::chrono::minutes{1};
    const auto request_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining) - clock_sample_margin;
    const sparenode::http::HttpConnectionHandlerConfig config{
        .timeouts = {.total = request_timeout},
        .deadline_provider = {},
    };

    send_all(pair.client, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto result =
        sparenode::http::handle_http_connection(std::move(pair.server), router, {}, config);

    REQUIRE(result.has_value());
    CHECK(receive_until_closed(pair.client).starts_with("HTTP/1.1 404 Not Found\r\n"));
}

TEST_CASE("HTTP connection session rejects the first timeout beyond its deadline range",
          "[http][session][limits][timeout]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    const auto remaining = (sparenode::network::NetworkDeadline::max)().time_since_epoch() -
                           std::chrono::steady_clock::now().time_since_epoch();
    const auto request_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(remaining) +
                                 std::chrono::milliseconds{1};
    const sparenode::http::HttpConnectionHandlerConfig config{
        .timeouts = {.total = request_timeout},
        .deadline_provider = {},
    };

    const auto result =
        sparenode::http::handle_http_connection(std::move(pair.server), router, {}, config);

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

    CHECK(response == "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
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

TEST_CASE("HTTP sessions expire idle partial headers and partial bodies",
          "[http][session][integration][timeout]")
{
    const std::string_view prefix =
        GENERATE("", "GET / HTTP/1.1\r\nHost: loc",
                 "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\nx",
                 "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nx");
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    const bool reading_body = prefix.starts_with("POST");
    const sparenode::http::HttpConnectionHandlerConfig config{
        .timeouts = {.headers =
                         reading_body ? std::chrono::seconds{30} : std::chrono::milliseconds{30},
                     .body =
                         reading_body ? std::chrono::milliseconds{30} : std::chrono::seconds{30},
                     .total = std::chrono::seconds{30}},
        .deadline_provider = {}};
    send_all(pair.client, prefix);
    const auto started = std::chrono::steady_clock::now();
    const auto result =
        sparenode::http::handle_http_connection(std::move(pair.server), router, {}, config);
    // A broad watchdog distinguishes phase expiry from the much later total cap.
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{5});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::timeout);
    CHECK(pair.client.peer_closes_within(std::chrono::seconds{1}));
}

TEST_CASE("A custom HTTP deadline cannot extend the configured total budget",
          "[http][session][integration][timeout]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    const sparenode::http::HttpConnectionHandlerConfig config{
        .timeouts = {.total = std::chrono::milliseconds{20}},
        .deadline_provider =
            [](sparenode::http::HttpRequestReadPhase, sparenode::network::NetworkDeadline started)
        { return started + std::chrono::seconds{10}; }};
    const auto started = std::chrono::steady_clock::now();
    const auto result =
        sparenode::http::handle_http_connection(std::move(pair.server), router, {}, config);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{5});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::timeout);
}

TEST_CASE("HTTP total deadline expires despite repeated receive progress",
          "[http][session][integration][timeout]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    std::size_t reads = 0;
    const sparenode::http::HttpConnectionHandlerConfig config{
        .receive_chunk_bytes = 1,
        .timeouts = {.headers = std::chrono::seconds{2},
                     .body = std::chrono::seconds{2},
                     .total = std::chrono::milliseconds{200}},
        .deadline_provider =
            [&](sparenode::http::HttpRequestReadPhase, sparenode::network::NetworkDeadline)
        {
            // Keep an incomplete request line readable across successive receives.
            send_all(pair.client, "G");
            ++reads;
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            return std::nullopt;
        }};
    const auto started = std::chrono::steady_clock::now();
    const auto result =
        sparenode::http::handle_http_connection(std::move(pair.server), router, {}, config);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{5});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::timeout);
    CHECK(result.error().operation == sparenode::network::NetworkOperation::receive);
    CHECK(reads > 1);
    // Unread trickle bytes may cause a reset instead of orderly EOF on close.
    std::array<std::byte, 1> buffer{};
    const auto closed = pair.client.receive_within(buffer, std::chrono::seconds{1});
    CHECK(sparenode::test::require_optional(closed) <= 0);
}

TEST_CASE("HTTP cancellation wins when the supplied deadline has already expired",
          "[http][session][integration][timeout][cancel]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    std::stop_source stop;
    stop.request_stop();
    const sparenode::http::HttpConnectionHandlerConfig config{
        .deadline_provider =
            [](sparenode::http::HttpRequestReadPhase, sparenode::network::NetworkDeadline)
        { return sparenode::network::NetworkDeadline{}; }};
    const auto result = sparenode::http::handle_http_connection(std::move(pair.server), router,
                                                                stop.get_token(), config);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::cancellation);
}

TEST_CASE("An HTTP timeout is logged and releases the sole worker for another request",
          "[http][session][integration][timeout][logging]")
{
    std::ostringstream output;
    const sparenode::logging::Logger logger(std::make_shared<sparenode::logging::ConsoleLogSink>(
        output, sparenode::logging::ConsoleColorMode::disabled));
    auto router = std::make_shared<sparenode::http::HttpRouter>();
    auto handler = sparenode::http::make_http_connection_handler(
        router, {.timeouts = {.headers = std::chrono::seconds{1}}, .deadline_provider = {}});
    std::promise<sparenode::network::ConnectionFailure> failure;
    auto observed = failure.get_future();
    auto observer = [log_failure = sparenode::logging::make_connection_failure_log_observer(logger),
                     &failure](const sparenode::network::ConnectionFailure &value)
    {
        log_failure(value);
        failure.set_value(value);
    };
    auto started = sparenode::network::ConnectionServer::start(
        {{"127.0.0.1", 0}, 128, false, {{1, 4}, std::move(handler), observer}, {}});
    REQUIRE(started.has_value());
    auto server = std::move(started).value();
    const auto endpoint = server.local_endpoint();
    auto idle = sparenode::test::connect_test_client(sparenode::test::require_optional(endpoint));
    auto active = sparenode::test::connect_test_client(sparenode::test::require_optional(endpoint));
    send_all(active, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(observed.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    const auto error = observed.get();
    CHECK(sparenode::test::require_optional(error.network_error).domain ==
          sparenode::network::NetworkErrorDomain::timeout);
    CHECK(receive_until_closed(active).starts_with("HTTP/1.1 404 Not Found\r\n"));
    server.request_stop();
    CHECK(output.str().contains("domain=timeout"));
}

TEST_CASE("HTTP session routes decoded chunked payload after trailers", "[http][session][chunked]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    std::string payload;
    REQUIRE(router.register_route(sparenode::http::HttpMethod::post, "/",
                                  [&](const sparenode::http::HttpRequestView &request,
                                      const sparenode::http::HttpRouteParameters &)
                                  {
                                      payload.assign(
                                          reinterpret_cast<const char *>(request.body().data()),
                                          request.body().size());
                                      return ok_response();
                                  }));
    send_all(pair.client, "POST / HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: Chunked\r\n\r\n"
                          "2;test=\"yes\"\r\nhe\r\n3\r\nllo\r\n0\r\nX-Check: ignored\r\n\r\n");
    const auto result = sparenode::http::handle_http_connection(
        std::move(pair.server), router, {}, {.receive_chunk_bytes = 1, .deadline_provider = {}});
    REQUIRE(result.has_value());
    CHECK(payload == "hello");
    CHECK(receive_until_closed(pair.client).starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST_CASE("HTTP session rejects premature body EOF without routing",
          "[http][session][chunked][eof]")
{
    const std::string_view framing =
        GENERATE("Content-Length: 4\r\n\r\nx", "Transfer-Encoding: chunked\r\n\r\n4\r\nx",
                 "Transfer-Encoding: chunked\r\n\r\n0\r\nX: y\r\n");
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    send_all(pair.client, std::string("POST / HTTP/1.1\r\nHost: local\r\n") + std::string(framing));
    pair.client.shutdown_send();
    const auto result = sparenode::http::handle_http_connection(std::move(pair.server), router, {});
    REQUIRE(result.has_value());
    CHECK(receive_until_closed(pair.client).starts_with("HTTP/1.1 400 Bad Request\r\n"));
}

TEST_CASE("HTTP chunked body wait remains cancellable", "[http][session][chunked][cancel]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    std::stop_source stop;
    send_all(pair.client,
             "POST / HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n1\r\n");
    const sparenode::http::HttpConnectionHandlerConfig config{
        .deadline_provider = [&](const sparenode::http::HttpRequestReadPhase phase,
                                 sparenode::network::NetworkDeadline)
        {
            if (phase == sparenode::http::HttpRequestReadPhase::body)
            {
                stop.request_stop();
            }
            return std::nullopt;
        }};
    const auto result = sparenode::http::handle_http_connection(std::move(pair.server), router,
                                                                stop.get_token(), config);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::cancellation);
}

TEST_CASE("HTTP session applies target and version policy on the wire", "[http][session][target]")
{
    const auto fixture =
        GENERATE(std::pair{"GET http://local?x=1 HTTP/1.9", "HTTP/1.1 200 OK\r\n"},
                 std::pair{"OPTIONS * HTTP/1.2", "HTTP/1.1 204 No Content\r\n"},
                 std::pair{"GET / HTTP/2.0", "HTTP/1.1 505 HTTP Version Not Supported\r\n"},
                 std::pair{"GET / HTTP/1.10", "HTTP/1.1 400 Bad Request\r\n"},
                 std::pair{"GET * HTTP/1.1", "HTTP/1.1 400 Bad Request\r\n"});
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    std::string routed_target;
    std::string routed_host;
    REQUIRE(router.register_route(sparenode::http::HttpMethod::get, "/",
                                  [&](const sparenode::http::HttpRequestView &request,
                                      const sparenode::http::HttpRouteParameters &)
                                  {
                                      routed_target = request.target();
                                      routed_host = request.header("Host");
                                      return ok_response();
                                  }));
    send_all(pair.client, std::string(fixture.first) + "\r\nHost: other\r\n\r\n");
    const auto result = sparenode::http::handle_http_connection(std::move(pair.server), router, {});
    REQUIRE(result.has_value());
    CHECK(receive_until_closed(pair.client).starts_with(fixture.second));
    if (std::string_view(fixture.first).starts_with("GET http:"))
    {
        CHECK(routed_target == "/?x=1");
        CHECK(routed_host == "local");
    }
    else
    {
        CHECK(routed_target.empty());
    }
}

TEST_CASE("HTTP close preserves final responses with unread socket input",
          "[http][session][integration][close]")
{
    const bool malformed = GENERATE(false, true);
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    const std::string request = malformed ? "GET /%GG HTTP/1.1\r\nHost: local\r\n\r\n"
                                          : "GET / HTTP/1.1\r\nHost: local\r\n\r\n";
    // Exceed the receive chunk so trailing bytes remain in the native socket.
    send_all(pair.client,
             request + "GET /second HTTP/1.1\r\nHost: local\r\n\r\n" + std::string(32768, 'x'));
    pair.client.shutdown_send();
    std::promise<SessionResult> promise;
    auto future = promise.get_future();
    std::jthread worker(
        [&]
        {
            promise.set_value(sparenode::http::handle_http_connection(
                std::move(pair.server), router, {},
                {.receive_chunk_bytes = 16, .deadline_provider = {}}));
        });
    const auto response = receive_until_closed(pair.client);
    REQUIRE(future.get());
    CHECK(response.starts_with(malformed ? "HTTP/1.1 400 " : "HTTP/1.1 404 "));
    CHECK(response.contains("Connection: close\r\n"));
    CHECK(response.ends_with("\r\n\r\n"));
    CHECK(response.find("HTTP/1.1", 1) == std::string::npos);
}

TEST_CASE("HTTP drain remains cancellable after the client observes FIN",
          "[http][session][integration][close]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    std::promise<SessionResult> promise;
    auto future = promise.get_future();
    std::jthread worker(
        [&](const std::stop_token &token)
        {
            promise.set_value(sparenode::http::handle_http_connection(
                std::move(pair.server), router, token,
                {.deadline_provider = {}, .drain_timeout = std::chrono::seconds{5}}));
        });
    send_all(pair.client, "GET / HTTP/1.1\r\nHost: local\r\n\r\n");
    CHECK(receive_until_closed(pair.client).contains("Connection: close\r\n"));
    CHECK(future.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
    worker.request_stop();
    REQUIRE(future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    const auto result = future.get();
    REQUIRE_FALSE(result);
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::cancellation);
}

TEST_CASE("HTTP drain stops at its byte budget while the peer stays open",
          "[http][session][integration][close]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    std::promise<SessionResult> promise;
    auto future = promise.get_future();
    constexpr std::size_t drain_limit = 32;
    std::jthread worker(
        [&](const std::stop_token &token)
        {
            promise.set_value(sparenode::http::handle_http_connection(
                std::move(pair.server), router, token,
                {.deadline_provider = {},
                 .max_drain_bytes = drain_limit,
                 .drain_timeout = std::chrono::seconds{5}}));
        });
    send_all(pair.client, "GET / HTTP/1.1\r\nHost: local\r\n\r\n");
    CHECK(receive_until_closed(pair.client).contains("Connection: close\r\n"));
    send_all(pair.client, std::string(drain_limit - 1, 'x'));
    CHECK(future.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
    send_all(pair.client, "x");
    REQUIRE(future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
    CHECK(future.get().has_value());
}

TEST_CASE("HTTP drain deadline is absolute for idle and trickling peers",
          "[http][session][integration][close]")
{
    const bool trickle = GENERATE(false, true);
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    std::promise<SessionResult> promise;
    auto future = promise.get_future();
    std::jthread worker(
        [&](const std::stop_token &token)
        {
            promise.set_value(sparenode::http::handle_http_connection(
                std::move(pair.server), router, token,
                {.deadline_provider = {}, .drain_timeout = std::chrono::milliseconds{100}}));
        });
    send_all(pair.client, "GET / HTTP/1.1\r\nHost: local\r\n\r\n");
    CHECK(receive_until_closed(pair.client).contains("Connection: close\r\n"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (future.wait_for(std::chrono::milliseconds{10}) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline)
    {
        if (trickle)
        {
            // A close racing this send is expected when the absolute budget expires.
            static_cast<void>(pair.client.send(bytes_of("x")));
        }
    }
    REQUIRE(future.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
    CHECK(future.get().has_value());
}

TEST_CASE("HTTP session rejects invalid close budgets before receiving",
          "[http][session][close][limits]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    sparenode::http::HttpConnectionHandlerConfig config;
    const auto invalid_policy = GENERATE(0, 1, 2, 3);
    if (invalid_policy == 0)
    {
        config.max_drain_bytes = 0;
    }
    else
    {
        config.drain_timeout = invalid_policy == 1   ? std::chrono::milliseconds{0}
                               : invalid_policy == 2 ? std::chrono::milliseconds{-1}
                                                     : std::chrono::hours{25};
    }
    const auto result =
        sparenode::http::handle_http_connection(std::move(pair.server), router, {}, config);
    REQUIRE_FALSE(result);
    CHECK(result.error().domain == sparenode::network::NetworkErrorDomain::validation);
}

TEST_CASE("HTTP session requires a final response from endpoint handlers", "[http][session][close]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    sparenode::http::HttpRouter router;
    REQUIRE(router.register_route(
        sparenode::http::HttpMethod::get, "/",
        [](const sparenode::http::HttpRequestView &, const sparenode::http::HttpRouteParameters &)
            -> sparenode::Result<sparenode::http::HttpResponse, sparenode::http::HttpRouteError>
        {
            auto response = sparenode::http::HttpResponse::create(
                static_cast<sparenode::http::HttpStatusCode>(103), "Early Hints", {}, {});
            return std::move(response).value();
        }));
    send_all(pair.client, "GET / HTTP/1.1\r\nHost: local\r\n\r\n");
    pair.client.shutdown_send();
    REQUIRE(sparenode::http::handle_http_connection(std::move(pair.server), router, {}));
    const auto response = receive_until_closed(pair.client);
    CHECK(response.starts_with("HTTP/1.1 500 "));
    CHECK(response.contains("Connection: close\r\n"));
    CHECK_FALSE(response.contains("103"));
}
