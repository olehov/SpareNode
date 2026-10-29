#include <cstddef>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/http/api/json_response.hpp"

TEST_CASE("JSON response factory owns canonical validated response state", "[http][api]")
{
    auto response = sparenode::http::api::make_json_response(
        sparenode::http::HttpStatusCode::unauthorized, "{\"error\":\"denied\"}",
        {{"content-type", "text/html"}, {"WWW-Authenticate", "SpareNodeSession"}});

    REQUIRE(response);
    CHECK(response->reason_phrase() == "Unauthorized");
    REQUIRE(response->headers().size() == 2);
    CHECK(response->headers()[0].name == "Content-Type");
    CHECK(response->headers()[0].value == "application/json; charset=utf-8");
    CHECK(response->headers()[1].name == "WWW-Authenticate");
    const auto body = response->memory_body();
    CHECK(std::string(reinterpret_cast<const char *>(body.data()), body.size()) ==
          "{\"error\":\"denied\"}");
}

TEST_CASE("JSON response factory preserves response validation failures", "[http][api]")
{
    const std::string oversized_body(sparenode::http::HttpResponse::maximum_memory_body_bytes + 1,
                                     'x');

    const auto response = sparenode::http::api::make_json_response(
        sparenode::http::HttpStatusCode::ok, oversized_body);

    REQUIRE_FALSE(response);
    CHECK(response.error().code ==
          sparenode::http::HttpRouteErrorCode::response_validation_failure);
    CHECK(
        response.error().detail ==
        static_cast<int>(sparenode::http::HttpResponseValidationErrorCode::memory_body_too_large));
}
