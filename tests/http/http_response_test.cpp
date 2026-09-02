#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparenode/http/http_response.hpp"
#include "sparenode/http/http_response_writer.hpp"
#include "support/connected_tcp_pair.hpp"
#include "support/optional.hpp"

namespace
{

using HttpStatusCode = sparenode::http::HttpStatusCode;

[[nodiscard]] std::vector<std::byte> bytes_of(const std::string_view text)
{
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    return {bytes.begin(), bytes.end()};
}

[[nodiscard]] std::string receive_exact(const sparenode::test::TestClientSocket &client,
                                        const std::size_t expected_size)
{
    std::vector<std::byte> received(expected_size);
    std::size_t offset = 0;
    while (offset < received.size())
    {
        const std::ptrdiff_t count = client.receive(std::span(received).subspan(offset));
        REQUIRE(count > 0);
        offset += static_cast<std::size_t>(count);
    }
    return {reinterpret_cast<const char *>(received.data()), received.size()};
}

[[nodiscard]] sparenode::http::HttpResponse make_memory_response(const std::string_view body)
{
    auto result = sparenode::http::HttpResponse::create(
        HttpStatusCode::ok, "OK", {{"Content-Type", "text/plain"}}, bytes_of(body));
    REQUIRE(result.has_value());
    return std::move(result).value();
}

} // namespace

TEST_CASE("HTTP response validates status fields and managed framing", "[http][response]")
{
    using Code = sparenode::http::HttpResponseValidationErrorCode;

    const HttpStatusCode invalid_status_code{};
    const auto invalid_status =
        sparenode::http::HttpResponse::create(invalid_status_code, "Invalid", {}, {});
    const auto invalid_reason =
        sparenode::http::HttpResponse::create(HttpStatusCode::ok, "OK\r\nInjected", {}, {});
    const auto invalid_name = sparenode::http::HttpResponse::create(HttpStatusCode::ok, "OK",
                                                                    {{"Bad Header", "value"}}, {});
    const auto invalid_value =
        sparenode::http::HttpResponse::create(HttpStatusCode::ok, "OK", {{"X-Test", "a\nb"}}, {});
    const auto content_length = sparenode::http::HttpResponse::create(
        HttpStatusCode::ok, "OK", {{"content-length", "0"}}, {});
    const auto forbidden_body = sparenode::http::HttpResponse::create(
        HttpStatusCode::no_content, "No Content", {}, bytes_of("x"));
    const auto forbidden_stream = sparenode::http::HttpResponse::create_streaming(
        HttpStatusCode::reset_content, "Reset Content", {}, 0,
        [](std::span<std::byte>, const std::stop_token &)
            -> sparenode::Result<std::size_t, sparenode::http::HttpBodyReadError> { return 0; });

    REQUIRE_FALSE(invalid_status.has_value());
    CHECK(invalid_status.error().code == Code::invalid_status_code);
    REQUIRE_FALSE(invalid_reason.has_value());
    CHECK(invalid_reason.error().code == Code::invalid_reason_phrase);
    REQUIRE_FALSE(invalid_name.has_value());
    CHECK(invalid_name.error().code == Code::invalid_header_name);
    REQUIRE_FALSE(invalid_value.has_value());
    CHECK(invalid_value.error().code == Code::invalid_header_value);
    REQUIRE_FALSE(content_length.has_value());
    CHECK(content_length.error().code == Code::managed_framing_header);
    REQUIRE_FALSE(forbidden_body.has_value());
    CHECK(forbidden_body.error().code == Code::body_not_allowed);
    REQUIRE_FALSE(forbidden_stream.has_value());
    CHECK(forbidden_stream.error().code == Code::body_not_allowed);
}

TEST_CASE("HTTP response enforces bounded owned state", "[http][response][limits]")
{
    using Code = sparenode::http::HttpResponseValidationErrorCode;

    std::vector<sparenode::http::HttpResponseHeader> headers(
        sparenode::http::HttpResponse::maximum_header_count + 1, {"X-Test", "value"});
    const auto too_many =
        sparenode::http::HttpResponse::create(HttpStatusCode::ok, "OK", std::move(headers), {});
    std::vector<std::byte> body(sparenode::http::HttpResponse::maximum_memory_body_bytes + 1);
    const auto too_large =
        sparenode::http::HttpResponse::create(HttpStatusCode::ok, "OK", {}, std::move(body));
    const auto head_too_large = sparenode::http::HttpResponse::create(
        HttpStatusCode::ok, std::string(sparenode::http::HttpResponse::maximum_head_bytes, 'r'), {},
        {});
    const auto missing_reader = sparenode::http::HttpResponse::create_streaming(
        HttpStatusCode::ok, "OK", {}, 1, sparenode::http::HttpBodyReader{});

    REQUIRE_FALSE(too_many.has_value());
    CHECK(too_many.error().code == Code::too_many_headers);
    REQUIRE_FALSE(too_large.has_value());
    CHECK(too_large.error().code == Code::memory_body_too_large);
    REQUIRE_FALSE(head_too_large.has_value());
    CHECK(head_too_large.error().code == Code::response_head_too_large);
    REQUIRE_FALSE(missing_reader.has_value());
    CHECK(missing_reader.error().code == Code::missing_body_reader);
}

TEST_CASE("HTTP response head serialization generates exact framing", "[http][response]")
{
    auto response = make_memory_response("hello");
    CHECK(sparenode::http::serialize_http_response_head(response) ==
          "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\n");

    auto no_content_result = sparenode::http::HttpResponse::create(
        HttpStatusCode::no_content, "No Content", {{"X-Test", "yes"}}, {});
    REQUIRE(no_content_result.has_value());
    CHECK(sparenode::http::serialize_http_response_head(no_content_result.value()) ==
          "HTTP/1.1 204 No Content\r\nX-Test: yes\r\n\r\n");
}

TEST_CASE("HTTP response writer sends an in-memory response completely",
          "[http][response][network]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    auto response = make_memory_response("hello");
    const std::string expected = sparenode::http::serialize_http_response_head(response) + "hello";

    const auto result = sparenode::http::write_http_response(pair.server, response);

    REQUIRE(result.has_value());
    CHECK(receive_exact(pair.client, expected.size()) == expected);
}

TEST_CASE("HTTP response writer streams through a fixed caller-bounded destination",
          "[http][response][streaming]")
{
    std::string payload(40000, 's');
    std::size_t source_offset = 0;
    std::size_t largest_destination = 0;
    auto response_result = sparenode::http::HttpResponse::create_streaming(
        HttpStatusCode::ok, "OK", {{"Content-Type", "application/octet-stream"}}, payload.size(),
        [&payload, &source_offset, &largest_destination](const std::span<std::byte> destination,
                                                         const std::stop_token &)
            -> sparenode::Result<std::size_t, sparenode::http::HttpBodyReadError>
        {
            largest_destination = (std::max)(largest_destination, destination.size());
            const std::size_t count =
                (std::min)(destination.size(), payload.size() - source_offset);
            for (std::size_t index = 0; index < count; ++index)
            {
                destination[index] = static_cast<std::byte>(payload[source_offset + index]);
            }
            source_offset += count;
            return count;
        });
    REQUIRE(response_result.has_value());
    auto response = std::move(response_result).value();
    const std::string expected = sparenode::http::serialize_http_response_head(response) + payload;
    auto pair = sparenode::test::create_connected_tcp_pair();

    const auto result = sparenode::http::write_http_response(pair.server, response);

    REQUIRE(result.has_value());
    CHECK(source_offset == payload.size());
    CHECK(largest_destination <= std::size_t{16} * 1024);
    CHECK(receive_exact(pair.client, expected.size()) == expected);
}

TEST_CASE("HTTP response writer preserves body source failures", "[http][response][error]")
{
    const sparenode::http::HttpBodyReadError expected_error{
        sparenode::http::HttpBodyReadErrorDomain::filesystem, 17};
    auto response_result = sparenode::http::HttpResponse::create_streaming(
        HttpStatusCode::ok, "OK", {}, 1,
        [expected_error](std::span<std::byte>, const std::stop_token &)
            -> sparenode::Result<std::size_t, sparenode::http::HttpBodyReadError>
        { return sparenode::unexpected(expected_error); });
    REQUIRE(response_result.has_value());
    auto response = std::move(response_result).value();
    auto pair = sparenode::test::create_connected_tcp_pair();

    const auto result = sparenode::http::write_http_response(pair.server, response);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == sparenode::http::HttpResponseWriteErrorCode::body_source_failure);
    const auto &body_error = sparenode::test::require_optional(result.error().body_error);
    CHECK(body_error.domain == expected_error.domain);
    CHECK(body_error.code == expected_error.code);
}

TEST_CASE("HTTP response writer rejects an early streaming EOF", "[http][response][error]")
{
    auto response_result = sparenode::http::HttpResponse::create_streaming(
        HttpStatusCode::ok, "OK", {}, 1,
        [](std::span<std::byte>, const std::stop_token &)
            -> sparenode::Result<std::size_t, sparenode::http::HttpBodyReadError> { return 0; });
    REQUIRE(response_result.has_value());
    auto response = std::move(response_result).value();
    auto pair = sparenode::test::create_connected_tcp_pair();

    const auto result = sparenode::http::write_http_response(pair.server, response);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == sparenode::http::HttpResponseWriteErrorCode::body_ended_early);
}

TEST_CASE("HTTP response writer rejects an invalid body source count", "[http][response][error]")
{
    auto response_result = sparenode::http::HttpResponse::create_streaming(
        HttpStatusCode::ok, "OK", {}, 1,
        [](const std::span<std::byte> destination, const std::stop_token &)
            -> sparenode::Result<std::size_t, sparenode::http::HttpBodyReadError>
        { return destination.size() + 1; });
    REQUIRE(response_result.has_value());
    auto response = std::move(response_result).value();
    auto pair = sparenode::test::create_connected_tcp_pair();

    const auto result = sparenode::http::write_http_response(pair.server, response);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code ==
          sparenode::http::HttpResponseWriteErrorCode::body_source_contract_violation);
}

TEST_CASE("HTTP response writer preserves a connection write failure",
          "[http][response][network][error]")
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    auto open_connection = std::move(pair.server);
    auto response = make_memory_response("hello");

    const auto result = sparenode::http::write_http_response(pair.server, response);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == sparenode::http::HttpResponseWriteErrorCode::network_failure);
    const auto &network_error = sparenode::test::require_optional(result.error().network_error);
    CHECK(network_error.domain == sparenode::network::NetworkErrorDomain::state);
    CHECK(open_connection.is_open());
}

TEST_CASE("HTTP response writer contains body source exceptions", "[http][response][error]")
{
    auto response_result = sparenode::http::HttpResponse::create_streaming(
        HttpStatusCode::ok, "OK", {}, 1,
        [](std::span<std::byte>, const std::stop_token &)
            -> sparenode::Result<std::size_t, sparenode::http::HttpBodyReadError> { throw 17; });
    REQUIRE(response_result.has_value());
    auto response = std::move(response_result).value();
    auto pair = sparenode::test::create_connected_tcp_pair();

    const auto result = sparenode::http::write_http_response(pair.server, response);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code ==
          sparenode::http::HttpResponseWriteErrorCode::body_source_exception);
}

TEST_CASE("HTTP response writer preserves cancellation before transmission",
          "[http][response][cancel]")
{
    auto response = make_memory_response("hello");
    auto pair = sparenode::test::create_connected_tcp_pair();
    std::stop_source stop_source;
    REQUIRE(stop_source.request_stop());

    const auto result =
        sparenode::http::write_http_response(pair.server, response, stop_source.get_token());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == sparenode::http::HttpResponseWriteErrorCode::network_failure);
    const auto &network_error = sparenode::test::require_optional(result.error().network_error);
    CHECK(network_error.domain == sparenode::network::NetworkErrorDomain::cancellation);
}

TEST_CASE("HTTP response error descriptions cover every category", "[http][response]")
{
    using Code = sparenode::http::HttpResponseWriteErrorCode;
    CHECK(std::string_view(sparenode::http::to_string(Code::network_failure)).starts_with("HTTP"));
    CHECK(
        std::string_view(sparenode::http::to_string(Code::connection_closed)).starts_with("HTTP"));
    CHECK(std::string_view(sparenode::http::to_string(Code::body_source_failure))
              .starts_with("HTTP"));
    CHECK(std::string_view(sparenode::http::to_string(Code::body_source_exception))
              .starts_with("HTTP"));
    CHECK(std::string_view(sparenode::http::to_string(Code::body_source_contract_violation))
              .starts_with("HTTP"));
    CHECK(std::string_view(sparenode::http::to_string(Code::body_ended_early)).starts_with("HTTP"));
    CHECK(std::string_view(sparenode::http::to_string(Code::resource_allocation_failed))
              .starts_with("HTTP"));
}
