#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "sparenode/authentication/api/origin_csrf_policy.hpp"
#include "sparenode/http/api/api_router_builder.hpp"
#include "sparenode/http/request/http_request_parser.hpp"
#include "support/optional.hpp"

namespace
{

/// @brief Parses one complete request for policy-focused dispatch.
[[nodiscard]] sparenode::http::HttpRequestView parse_request(const std::string_view source)
{
    auto parsed =
        sparenode::http::parse_http_request(std::as_bytes(std::span(source.data(), source.size())));
    REQUIRE(parsed);
    return sparenode::test::require_optional(parsed->request());
}

/// @brief Converts one owned response body into comparable text.
[[nodiscard]] std::string body_text(const sparenode::http::HttpResponse &response)
{
    const auto body = response.memory_body();
    return {reinterpret_cast<const char *>(body.data()), body.size()};
}

/// @brief Creates a terminal route handler that records policy acceptance.
[[nodiscard]] sparenode::http::HttpRouteHandler successful_handler(bool &handler_called)
{
    return [&handler_called](const sparenode::http::HttpRequestView &,
                             const sparenode::http::HttpRouteParameters &)
               -> sparenode::Result<sparenode::http::HttpResponse, sparenode::http::HttpRouteError>
    {
        handler_called = true;
        auto response = sparenode::http::HttpResponse::create(
            sparenode::http::HttpStatusCode::no_content, "No Content", {}, {});
        REQUIRE(response);
        return std::move(response).value();
    };
}

/// @brief Builds a protected route whose terminal handler records policy acceptance.
[[nodiscard]] sparenode::http::HttpRouter make_router(bool &handler_called)
{
    sparenode::http::api::ProtectedRoutePolicies policies;
    policies.origin_csrf = sparenode::authentication::api::make_origin_csrf_policy();
    sparenode::http::api::ApiRouterBuilder builder(
        sparenode::http::api::make_protected_route_decorator(std::move(policies)));
    REQUIRE(builder.register_protected(
        {sparenode::http::HttpMethod::post, "/mutation", successful_handler(handler_called)}));
    return std::move(builder).build();
}

/// @brief Dispatches one POST request with caller-supplied browser security metadata.
[[nodiscard]] sparenode::http::HttpResponse dispatch(const sparenode::http::HttpRouter &router,
                                                     const std::string_view headers)
{
    const std::string request = "POST /mutation HTTP/1.1\r\nHost: localhost\r\n" +
                                std::string(headers) + "Content-Length: 0\r\n\r\n";
    auto response = router.dispatch(parse_request(request));
    REQUIRE(response);
    return std::move(response).value();
}

} // namespace

TEST_CASE("Origin CSRF policy accepts marked same-origin browser mutations",
          "[authentication][csrf][origin]")
{
    bool handler_called{};
    const auto router = make_router(handler_called);

    const auto response = dispatch(router, "X-SpareNode-CSRF: 1\r\n"
                                           "Origin: http://LOCALHOST:80\r\n"
                                           "Sec-Fetch-Site: same-origin\r\n");

    CHECK(response.status_code() == sparenode::http::HttpStatusCode::no_content);
    CHECK(handler_called);
}

TEST_CASE("Origin CSRF policy accepts a marked non-browser fallback without Origin",
          "[authentication][csrf][origin]")
{
    bool handler_called{};
    const auto router = make_router(handler_called);

    const auto response = dispatch(router, "X-SpareNode-CSRF: 1\r\n");

    CHECK(response.status_code() == sparenode::http::HttpStatusCode::no_content);
    CHECK(handler_called);
}

TEST_CASE("Origin CSRF policy rejects missing ambiguous and cross-site evidence",
          "[authentication][csrf][origin]")
{
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): Catch2 assertion internals.
    for (const std::string_view headers :
         {"", "X-SpareNode-CSRF: wrong\r\n", "X-SpareNode-CSRF: 1\r\nX-SpareNode-CSRF: 1\r\n",
          "X-SpareNode-CSRF: 1\r\nOrigin: https://localhost\r\n",
          "X-SpareNode-CSRF: 1\r\nOrigin: http://cross-site.example\r\n",
          "X-SpareNode-CSRF: 1\r\nOrigin: null\r\n",
          "X-SpareNode-CSRF: 1\r\nOrigin: http://localhost\r\nOrigin: http://localhost\r\n",
          "X-SpareNode-CSRF: 1\r\nSec-Fetch-Site: cross-site\r\n",
          "X-SpareNode-CSRF: 1\r\nSec-Fetch-Site: same-origin\r\nSec-Fetch-Site: same-origin\r\n"})
    {
        bool handler_called{};
        const auto router = make_router(handler_called);
        const auto response = dispatch(router, headers);

        CHECK(response.status_code() == sparenode::http::HttpStatusCode::forbidden);
        CHECK(body_text(response) == R"({"error":"csrf_validation_failed"})");
        CHECK_FALSE(handler_called);
    }
}
