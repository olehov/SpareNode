#include <cstddef>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/http/api/json_response.hpp"

TEST_CASE("JSON response factory owns canonical validated response state", "[http][api]")
{
    auto response = sparenode::http::api::make_json_response(
        sparenode::http::HttpStatusCode::unauthorized, "{\"error\":\"denied\"}",
        {{"WWW-Authenticate", "SpareNodeSession"}});

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
