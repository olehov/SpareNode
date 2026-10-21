#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/http/api/api_router_builder.hpp"
#include "sparenode/http/http_status_code.hpp"
#include "sparenode/http/request/http_request_parser.hpp"
#include "sparenode/http/response/http_response.hpp"
#include "support/optional.hpp"

namespace
{

/// @brief Parses one complete request whose source storage remains alive.
[[nodiscard]] sparenode::http::HttpRequestView parse_request(const std::string_view source)
{
    const auto bytes = std::as_bytes(std::span(source.data(), source.size()));
    auto parsed = sparenode::http::parse_http_request(bytes);
    REQUIRE(parsed);
    REQUIRE(parsed->is_complete());
    return sparenode::test::require_optional(parsed->request());
}

/// @brief Creates a route handler that returns the requested fixed status.
[[nodiscard]] sparenode::http::HttpRouteHandler
status_handler(const sparenode::http::HttpStatusCode status, std::string reason)
{
    return [status, reason = std::move(reason)](const sparenode::http::HttpRequestView &,
                                                const sparenode::http::HttpRouteParameters &)
               -> sparenode::Result<sparenode::http::HttpResponse, sparenode::http::HttpRouteError>
    {
        auto response =
            sparenode::http::HttpResponse::create(status, reason, {}, std::vector<std::byte>{});
        REQUIRE(response);
        return std::move(response).value();
    };
}

} // namespace

TEST_CASE("API router builder applies policy only to protected routes", "[http][api][routing]")
{
    sparenode::http::api::ProtectedRouteDecorator decorator = [](sparenode::http::HttpRouteHandler)
    { return status_handler(sparenode::http::HttpStatusCode::forbidden, "Forbidden"); };
    sparenode::http::api::ApiRouterBuilder builder(std::move(decorator));
    REQUIRE(builder.register_public({sparenode::http::HttpMethod::get, "/health",
                                     status_handler(sparenode::http::HttpStatusCode::ok, "OK")}));
    REQUIRE(
        builder.register_protected({sparenode::http::HttpMethod::get, "/files",
                                    status_handler(sparenode::http::HttpStatusCode::ok, "OK")}));
    REQUIRE(builder.public_routes().size() == 1);
    CHECK(builder.public_routes().front().pattern == "/health");
    REQUIRE(builder.protected_routes().size() == 1);
    CHECK(builder.protected_routes().front().pattern == "/files");
    auto router = std::move(builder).build();

    auto public_response =
        router.dispatch(parse_request("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    REQUIRE(public_response);
    CHECK(public_response->status_code() == sparenode::http::HttpStatusCode::ok);

    auto protected_response =
        router.dispatch(parse_request("GET /files HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    REQUIRE(protected_response);
    CHECK(protected_response->status_code() == sparenode::http::HttpStatusCode::forbidden);
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): std::move_only_function false positive.

TEST_CASE("Protected route policies execute in the fixed security order", "[http][api][policy]")
{
    std::vector<std::string> events;
    const auto record = [&events](std::string name)
    {
        return sparenode::http::api::ApiRequestDecorator(
            [&events, name = std::move(name)](sparenode::http::api::ApiRequestHandler next)
            {
                return sparenode::http::api::ApiRequestHandler(
                    [&events, name,
                     next = std::move(next)](sparenode::http::api::ApiRequestContext &context)
                    {
                        events.push_back(name);
                        return next(context);
                    });
            });
    };
    sparenode::http::api::ProtectedRoutePolicies policies{
        .transport = record("transport"),
        .session_authentication =
            [&events](sparenode::http::api::ApiRequestHandler next)
        {
            return sparenode::http::api::ApiRequestHandler(
                [&events, next = std::move(next)](sparenode::http::api::ApiRequestContext &context)
                {
                    events.emplace_back("session");
                    context.authenticated_principal = "owner";
                    return next(context);
                });
        },
        .origin_csrf = record("origin"),
        .authorization =
            [&events](sparenode::http::api::ApiRequestHandler next)
        {
            return sparenode::http::api::ApiRequestHandler(
                [&events, next = std::move(next)](sparenode::http::api::ApiRequestContext &context)
                {
                    events.emplace_back("authorization");
                    REQUIRE(context.authenticated_principal == "owner");
                    return next(context);
                });
        },
        .throttling = record("throttling"),
        .resource_coordination =
            [&events](sparenode::http::api::ApiRequestHandler next)
        {
            return sparenode::http::api::ApiRequestHandler(
                [&events, next = std::move(next)](sparenode::http::api::ApiRequestContext &context)
                {
                    events.emplace_back("coordination-enter");
                    auto response = next(context);
                    events.emplace_back("coordination-exit");
                    return response;
                });
        }};
    auto decorator = sparenode::http::api::make_protected_route_decorator(std::move(policies));
    sparenode::http::api::ApiRouterBuilder builder(std::move(decorator));
    REQUIRE(builder.register_protected(
        {sparenode::http::HttpMethod::get, "/files/*",
         [&events](const sparenode::http::HttpRequestView &,
                   const sparenode::http::HttpRouteParameters &)
             -> sparenode::Result<sparenode::http::HttpResponse, sparenode::http::HttpRouteError>
         {
             events.emplace_back("handler");
             auto response = sparenode::http::HttpResponse::create(
                 sparenode::http::HttpStatusCode::ok, "OK", {}, {});
             REQUIRE(response);
             return std::move(response).value();
         }}));
    auto router = std::move(builder).build();

    const auto response =
        router.dispatch(parse_request("GET /files/report HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    REQUIRE(response);
    CHECK(response->status_code() == sparenode::http::HttpStatusCode::ok);
    CHECK(events == std::vector<std::string>{"transport", "session", "origin", "authorization",
                                             "throttling", "coordination-enter", "handler",
                                             "coordination-exit"});
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): std::move_only_function false positive.

TEST_CASE("Protected route policies short-circuit before the operation handler",
          "[http][api][policy]")
{
    bool handler_called = false;
    sparenode::http::api::ProtectedRoutePolicies policies;
    policies.session_authentication = [](sparenode::http::api::ApiRequestHandler)
    {
        return sparenode::http::api::ApiRequestHandler(
            [](sparenode::http::api::ApiRequestContext &)
            {
                auto response = sparenode::http::HttpResponse::create(
                    sparenode::http::HttpStatusCode::unauthorized, "Unauthorized", {}, {});
                REQUIRE(response);
                return sparenode::Result<sparenode::http::HttpResponse,
                                         sparenode::http::HttpRouteError>(
                    std::move(response).value());
            });
    };
    sparenode::http::api::ApiRouterBuilder builder(
        sparenode::http::api::make_protected_route_decorator(std::move(policies)));
    REQUIRE(builder.register_protected(
        {sparenode::http::HttpMethod::get, "/files",
         [&handler_called](const sparenode::http::HttpRequestView &,
                           const sparenode::http::HttpRouteParameters &)
             -> sparenode::Result<sparenode::http::HttpResponse, sparenode::http::HttpRouteError>
         {
             handler_called = true;
             auto response = sparenode::http::HttpResponse::create(
                 sparenode::http::HttpStatusCode::ok, "OK", {}, {});
             REQUIRE(response);
             return std::move(response).value();
         }}));
    auto router = std::move(builder).build();

    const auto response =
        router.dispatch(parse_request("GET /files HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    REQUIRE(response);
    CHECK(response->status_code() == sparenode::http::HttpStatusCode::unauthorized);
    CHECK_FALSE(handler_called);
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): std::move_only_function false positive.

TEST_CASE("API router builder leaves protected handlers unchanged without a policy",
          "[http][api][routing]")
{
    sparenode::http::api::ApiRouterBuilder builder;
    REQUIRE(
        builder.register_protected({sparenode::http::HttpMethod::get, "/files",
                                    status_handler(sparenode::http::HttpStatusCode::ok, "OK")}));
    auto router = std::move(builder).build();

    auto response =
        router.dispatch(parse_request("GET /files HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    REQUIRE(response);
    CHECK(response->status_code() == sparenode::http::HttpStatusCode::ok);
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): std::move_only_function false positive.

TEST_CASE("API router builder preserves route registration failures", "[http][api][routing]")
{
    sparenode::http::api::ApiRouterBuilder builder;
    const auto invalid_public =
        builder.register_public({sparenode::http::HttpMethod::get, "relative",
                                 status_handler(sparenode::http::HttpStatusCode::ok, "OK")});
    REQUIRE_FALSE(invalid_public);
    CHECK(invalid_public.error().code ==
          sparenode::http::HttpRouteRegistrationErrorCode::invalid_pattern);

    REQUIRE(
        builder.register_protected({sparenode::http::HttpMethod::get, "/duplicate",
                                    status_handler(sparenode::http::HttpStatusCode::ok, "OK")}));
    const auto duplicate_protected =
        builder.register_protected({sparenode::http::HttpMethod::get, "/duplicate",
                                    status_handler(sparenode::http::HttpStatusCode::ok, "OK")});
    REQUIRE_FALSE(duplicate_protected);
    CHECK(duplicate_protected.error().code ==
          sparenode::http::HttpRouteRegistrationErrorCode::duplicate_route);
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): std::move_only_function false positive.
