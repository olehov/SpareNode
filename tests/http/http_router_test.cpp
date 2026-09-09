#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/http/http_request_parser.hpp"
#include "sparenode/http/http_router.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/result.hpp"
#include "support/optional.hpp"

namespace
{

/// @brief Exposes string storage as immutable bytes for parser tests.
/// @param[in] source Test request text.
/// @return Borrowed byte view over the same storage.
[[nodiscard]] std::span<const std::byte> as_bytes(const std::string_view source)
{
    return std::as_bytes(std::span(source.data(), source.size()));
}

/// @brief Parses one complete valid request fixture.
/// @param[in] source Request text whose storage outlives the returned view.
/// @return Complete borrowed request view.
[[nodiscard]] sparenode::http::HttpRequestView parse_request(const std::string_view source)
{
    auto parsed = sparenode::http::parse_http_request(as_bytes(source));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->is_complete());
    return sparenode::test::require_optional(parsed->request());
}

/// @brief Creates one empty validated response for router handler tests.
/// @param[in] status Response status.
/// @param[in] reason Response reason phrase.
/// @return Valid response or a structured invariant failure.
[[nodiscard]] sparenode::Result<sparenode::http::HttpResponse, sparenode::http::HttpRouteError>
make_response(const sparenode::http::HttpStatusCode status, std::string reason)
{
    auto response = sparenode::http::HttpResponse::create(status, std::move(reason), {}, {});
    if (!response)
    {
        return sparenode::unexpected(sparenode::http::HttpRouteError{
            sparenode::http::HttpRouteErrorCode::response_validation_failure, 0});
    }
    return std::move(response).value();
}

/// @brief Builds a handler that returns one fixed empty response.
/// @param[in] status Response status returned by the handler.
/// @param[in] reason Response reason phrase returned by the handler.
/// @return Const-callable move-only test handler.
[[nodiscard]] sparenode::http::HttpRouteHandler
respond_with(const sparenode::http::HttpStatusCode status, std::string reason)
{
    return [status, reason = std::move(reason)](const sparenode::http::HttpRequestView &,
                                                const sparenode::http::HttpRouteParameters &)
    { return make_response(status, reason); };
}

} // namespace

TEST_CASE("Origin and absolute forms select the same route and wildcard suffix",
          "[http][router][target]")
{
    sparenode::http::HttpRouter router;
    std::vector<std::string> selected;
    REQUIRE(router.register_route(sparenode::http::HttpMethod::get, "/files/*",
                                  [&](const sparenode::http::HttpRequestView &request,
                                      const sparenode::http::HttpRouteParameters &parameters)
                                  {
                                      selected.push_back(std::string(request.target()) + "|" +
                                                         std::string(parameters.wildcard_suffix()));
                                      return make_response(sparenode::http::HttpStatusCode::ok,
                                                           "OK");
                                  }));
    const std::string origin = "GET /files/a%2Fb?x=1 HTTP/1.1\r\nHost: local\r\n\r\n";
    const std::string absolute =
        "GET http://local/files/a%2Fb?x=1 HTTP/1.9\r\nHost: different\r\n\r\n";
    REQUIRE(router.dispatch(parse_request(origin)).has_value());
    REQUIRE(router.dispatch(parse_request(absolute)).has_value());
    // Two entries prove that both requests invoked the handler independently.
    REQUIRE(selected.size() == 2);
    CHECK(selected.front() == "/files/a%2Fb?x=1|a%2Fb");
    CHECK(selected.back() == selected.front());
}

TEST_CASE("OPTIONS asterisk is server-wide and bypasses resource routes", "[http][router][target]")
{
    sparenode::http::HttpRouter router;
    const std::string source = "OPTIONS * HTTP/1.1\r\nHost: local\r\n\r\n";
    auto response = router.dispatch(parse_request(source));
    REQUIRE(response.has_value());
    CHECK(response->status_code() == sparenode::http::HttpStatusCode::no_content);
    bool called = false;
    REQUIRE(router.register_route(
        sparenode::http::HttpMethod::options, "/*",
        [&](const sparenode::http::HttpRequestView &, const sparenode::http::HttpRouteParameters &)
        {
            called = true;
            return make_response(sparenode::http::HttpStatusCode::ok, "OK");
        }));
    response = router.dispatch(parse_request(source));
    REQUIRE(response.has_value());
    CHECK(response->status_code() == sparenode::http::HttpStatusCode::no_content);
    CHECK_FALSE(called);
}

TEST_CASE("HTTP router registers the initial SpareNode API route shapes",
          "[http][router][registration]")
{
    sparenode::http::HttpRouter router;
    CHECK(router.register_route(sparenode::http::HttpMethod::get, "/api/files",
                                respond_with(sparenode::http::HttpStatusCode::ok, "OK")));
    CHECK(router.register_route(sparenode::http::HttpMethod::get, "/api/file/*",
                                respond_with(sparenode::http::HttpStatusCode::ok, "OK")));
    CHECK(router.register_route(sparenode::http::HttpMethod::post, "/api/file/*",
                                respond_with(sparenode::http::HttpStatusCode::created, "Created")));
    CHECK(router.register_route(sparenode::http::HttpMethod::post, "/api/login",
                                respond_with(sparenode::http::HttpStatusCode::ok, "OK")));
    CHECK(router.register_route(sparenode::http::HttpMethod::get, "/api/status",
                                respond_with(sparenode::http::HttpStatusCode::ok, "OK")));
    CHECK(router.size() == 5);
}

TEST_CASE("HTTP router dispatches exact routes before wildcard routes", "[http][router][dispatch]")
{
    sparenode::http::HttpRouter router;
    REQUIRE(router.register_route(sparenode::http::HttpMethod::get, "/api/file/*",
                                  respond_with(sparenode::http::HttpStatusCode::ok, "Wildcard")));
    REQUIRE(router.register_route(
        sparenode::http::HttpMethod::get, "/api/file/metadata",
        respond_with(sparenode::http::HttpStatusCode::no_content, "No Content")));

    const auto request =
        parse_request("GET /api/file/metadata?details=true HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto response = router.dispatch(request);
    REQUIRE(response.has_value());
    CHECK(response->status_code() == sparenode::http::HttpStatusCode::no_content);
}

TEST_CASE("HTTP router exposes the longest terminal wildcard suffix", "[http][router][parameters]")
{
    sparenode::http::HttpRouter router;
    std::string captured;
    REQUIRE(router.register_route(
        sparenode::http::HttpMethod::get, "/api/*",
        [&captured](const sparenode::http::HttpRequestView &,
                    const sparenode::http::HttpRouteParameters &parameters)
        {
            captured = parameters.wildcard_suffix();
            return make_response(sparenode::http::HttpStatusCode::ok, "General");
        }));
    REQUIRE(
        router.register_route(sparenode::http::HttpMethod::get, "/api/file/*",
                              [&captured](const sparenode::http::HttpRequestView &,
                                          const sparenode::http::HttpRouteParameters &parameters)
                              {
                                  captured = parameters.wildcard_suffix();
                                  return make_response(sparenode::http::HttpStatusCode::ok, "File");
                              }));

    const auto request = parse_request("GET /api/file/folder/report.txt?download=true HTTP/1.1\r\n"
                                       "Host: localhost\r\n\r\n");
    const auto response = router.dispatch(request);
    REQUIRE(response.has_value());
    CHECK(response->reason_phrase() == "File");
    CHECK(captured == "folder/report.txt");
}

TEST_CASE("HTTP router creates standard responses for unmatched requests",
          "[http][router][dispatch]")
{
    sparenode::http::HttpRouter router;
    REQUIRE(
        router.register_route(sparenode::http::HttpMethod::post, "/api/file/*",
                              respond_with(sparenode::http::HttpStatusCode::created, "Created")));
    REQUIRE(router.register_route(sparenode::http::HttpMethod::get, "/api/file/*",
                                  respond_with(sparenode::http::HttpStatusCode::ok, "OK")));

    const auto missing = parse_request("GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto not_found = router.dispatch(missing);
    REQUIRE(not_found.has_value());
    CHECK(not_found->status_code() == sparenode::http::HttpStatusCode::not_found);

    const auto wrong_method =
        parse_request("DELETE /api/file/report.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto method_not_allowed = router.dispatch(wrong_method);
    REQUIRE(method_not_allowed.has_value());
    CHECK(method_not_allowed->status_code() == sparenode::http::HttpStatusCode::method_not_allowed);
    REQUIRE(method_not_allowed->headers().size() == 1);
    CHECK(method_not_allowed->headers().front().name == "Allow");
    CHECK(method_not_allowed->headers().front().value == "GET, HEAD, POST");
}

TEST_CASE("HTTP router preserves structured handler failures", "[http][router][errors]")
{
    sparenode::http::HttpRouter router;
    REQUIRE(router.register_route(
        sparenode::http::HttpMethod::get, "/api/status",
        [](const sparenode::http::HttpRequestView &, const sparenode::http::HttpRouteParameters &)
            -> sparenode::Result<sparenode::http::HttpResponse, sparenode::http::HttpRouteError>
        {
            return sparenode::unexpected(sparenode::http::HttpRouteError{
                sparenode::http::HttpRouteErrorCode::handler_failure, 17});
        }));

    const auto request = parse_request("GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto response = router.dispatch(request);
    REQUIRE(!response.has_value());
    CHECK(response.error().code == sparenode::http::HttpRouteErrorCode::handler_failure);
    CHECK(response.error().detail == 17);
}

TEST_CASE("HTTP router rejects invalid and duplicate registrations",
          "[http][router][registration][errors]")
{
    sparenode::http::HttpRouter router;
    const std::array invalid_patterns{
        "",        "api/status", "/api?query", "/api#fragment", "/api/*/nested", "/api*",
        "/api/**", "/bad path",  "/bad\\path", "/bad%2",        "/bad%GG",       "/bad\npath"};
    for (const std::string_view pattern : invalid_patterns)
    {
        DYNAMIC_SECTION(pattern)
        {
            const auto result =
                router.register_route(sparenode::http::HttpMethod::get, std::string(pattern),
                                      respond_with(sparenode::http::HttpStatusCode::ok, "OK"));
            REQUIRE(!result.has_value());
            CHECK(result.error().code ==
                  sparenode::http::HttpRouteRegistrationErrorCode::invalid_pattern);
        }
    }

    // The out-of-range value intentionally exercises the defensive public API boundary.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto unsupported_method = static_cast<sparenode::http::HttpMethod>(255);
    const auto invalid_method = router.register_route(
        unsupported_method, "/api/status", respond_with(sparenode::http::HttpStatusCode::ok, "OK"));
    REQUIRE(!invalid_method.has_value());
    CHECK(invalid_method.error().code ==
          sparenode::http::HttpRouteRegistrationErrorCode::invalid_method);

    const auto empty_handler =
        router.register_route(sparenode::http::HttpMethod::get, "/api/status", {});
    REQUIRE(!empty_handler.has_value());
    CHECK(empty_handler.error().code ==
          sparenode::http::HttpRouteRegistrationErrorCode::empty_handler);

    REQUIRE(router.register_route(sparenode::http::HttpMethod::get, "/api/status",
                                  respond_with(sparenode::http::HttpStatusCode::ok, "OK")));
    const auto duplicate =
        router.register_route(sparenode::http::HttpMethod::get, "/api/status",
                              respond_with(sparenode::http::HttpStatusCode::ok, "OK"));
    REQUIRE(!duplicate.has_value());
    CHECK(duplicate.error().code ==
          sparenode::http::HttpRouteRegistrationErrorCode::duplicate_route);
}

TEST_CASE("HTTP router enforces its route table boundary", "[http][router][limits]")
{
    sparenode::http::HttpRouter router;
    for (std::size_t index = 0; index < sparenode::http::HttpRouter::maximum_route_count; ++index)
    {
        REQUIRE(router.register_route(sparenode::http::HttpMethod::get,
                                      "/route/" + std::to_string(index),
                                      respond_with(sparenode::http::HttpStatusCode::ok, "OK")));
    }

    const auto overflow =
        router.register_route(sparenode::http::HttpMethod::get, "/overflow",
                              respond_with(sparenode::http::HttpStatusCode::ok, "OK"));
    REQUIRE(!overflow.has_value());
    CHECK(overflow.error().code ==
          sparenode::http::HttpRouteRegistrationErrorCode::too_many_routes);
}

TEST_CASE("HEAD routing preserves specificity and prefers explicit HEAD on ties",
          "[http][router][head]")
{
    using sparenode::http::HttpMethod;
    using sparenode::http::HttpStatusCode;
    for (const bool reverse : {false, true})
    {
        sparenode::http::HttpRouter router;
        const auto add_get = [&]
        {
            REQUIRE(router.register_route(HttpMethod::get, "/files/*",
                                          respond_with(HttpStatusCode::ok, "GET wildcard")));
            REQUIRE(router.register_route(HttpMethod::get, "/files/exact",
                                          respond_with(HttpStatusCode::ok, "GET exact")));
            REQUIRE(router.register_route(HttpMethod::get, "/files/deep/*",
                                          respond_with(HttpStatusCode::ok, "GET deep")));
        };
        const auto add_head = [&]
        {
            REQUIRE(router.register_route(HttpMethod::head, "/files/*",
                                          respond_with(HttpStatusCode::ok, "HEAD wildcard")));
        };
        if (reverse)
        {
            add_head();
            add_get();
        }
        else
        {
            add_get();
            add_head();
        }
        const auto wildcard =
            router.dispatch(parse_request("HEAD /files/item HTTP/1.1\r\nHost: local\r\n\r\n"));
        const auto exact =
            router.dispatch(parse_request("HEAD /files/exact HTTP/1.1\r\nHost: local\r\n\r\n"));
        const auto deep =
            router.dispatch(parse_request("HEAD /files/deep/item HTTP/1.1\r\nHost: local\r\n\r\n"));
        REQUIRE(wildcard);
        REQUIRE(exact);
        REQUIRE(deep);
        CHECK(wildcard->reason_phrase() == "HEAD wildcard");
        CHECK(exact->reason_phrase() == "GET exact");
        CHECK(deep->reason_phrase() == "GET deep");
        REQUIRE(router.register_route(HttpMethod::head, "/files/exact",
                                      respond_with(HttpStatusCode::ok, "HEAD exact")));
        const auto overridden =
            router.dispatch(parse_request("HEAD /files/exact HTTP/1.1\r\nHost: local\r\n\r\n"));
        REQUIRE(overridden);
        CHECK(overridden->reason_phrase() == "HEAD exact");
        const auto get =
            router.dispatch(parse_request("GET /files/exact HTTP/1.1\r\nHost: local\r\n\r\n"));
        REQUIRE(get);
        CHECK(get->reason_phrase() == "GET exact");
    }
}

TEST_CASE("A HEAD-only route does not implicitly allow GET", "[http][router][head]")
{
    sparenode::http::HttpRouter router;
    REQUIRE(router.register_route(sparenode::http::HttpMethod::head, "/",
                                  respond_with(sparenode::http::HttpStatusCode::ok, "HEAD only")));
    const auto get = router.dispatch(parse_request("GET / HTTP/1.1\r\nHost: local\r\n\r\n"));
    REQUIRE(get);
    CHECK(get->status_code() == sparenode::http::HttpStatusCode::method_not_allowed);
    REQUIRE(get->headers().size() == 1);
    CHECK(get->headers().front().value == "HEAD");
}
