#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/http/http_status_code.hpp"

TEST_CASE("HTTP statuses have one canonical reason phrase owner", "[http][response]")
{
    using sparenode::http::http_reason_phrase;
    using sparenode::http::HttpStatusCode;

    CHECK(http_reason_phrase(HttpStatusCode::ok) == "OK");
    CHECK(http_reason_phrase(HttpStatusCode::content_too_large) == "Content Too Large");
    CHECK(http_reason_phrase(HttpStatusCode::service_unavailable) == "Service Unavailable");
}
