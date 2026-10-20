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
