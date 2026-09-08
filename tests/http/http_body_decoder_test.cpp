#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "sparenode/http/detail/buffered_request.hpp"
#include "sparenode/http/http_body_decoder.hpp"
#include "support/optional.hpp"
#include <array>
#include <string>

namespace
{
/// @brief Borrows request fixture bytes without copying.
std::span<const std::byte> bytes(const std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
}
/// @brief Copies decoded output into a test-owned string.
std::string text(const std::span<const std::byte> data)
{
    return {reinterpret_cast<const char *>(data.data()), data.size()};
}
constexpr std::string_view head =
    "POST / HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n";
} // namespace

TEST_CASE("Chunk decoder preserves payload and exact boundary at every wire split",
          "[http][chunked]")
{
    const std::string wire = "4;name=token;quoted=\"a\\\"b\"\r\nWiki\r\n5\r\npedia\r\n0;end\r\nX-"
                             "Checksum: ignored\r\n\r\n";
    for (std::size_t split = 0; split <= wire.size(); ++split)
    {
        CAPTURE(split);
        sparenode::http::HttpBodyDecoder decoder({.chunked = true}, {});
        std::string decoded;
        for (auto piece :
             {std::string_view(wire).substr(0, split), std::string_view(wire).substr(split)})
        {
            while (!piece.empty())
            {
                std::array<std::byte, 2> output{};
                const auto progress = decoder.feed(bytes(piece), output);
                REQUIRE(progress.has_value());
                REQUIRE(progress->consumed > 0);
                decoded += text(std::span(output).first(progress->produced));
                piece.remove_prefix(progress->consumed);
            }
        }
        CHECK(decoded == "Wikipedia");
        CHECK(decoder.complete());
        CHECK(decoder.finish().has_value());
        std::array<std::byte, 1> output{};
        const auto trailing = decoder.feed(bytes("NEXT"), output);
        REQUIRE(trailing.has_value());
        CHECK(trailing->consumed == 0);
    }
}

TEST_CASE("Chunk decoder rejects malformed syntax and retains its terminal error",
          "[http][chunked][security]")
{
    const std::string wire =
        GENERATE("+1\r\nx\r\n0\r\n\r\n", "-1\r\n", "0x1\r\n", "\r\n",
                 "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\r\n", "1\nx", "1\rX", "1\r\nxZ", "1\r\nx\rZ",
                 "1;\r\n", "1;name=\r\n", "1;name=\"unterminated\r\n", "1 \r\n",
                 "1;name=\"a\"junk\r\n", "0\r\n folded: x\r\n\r\n", "0\r\nBad : x\r\n\r\n",
                 "0\r\nContent-Length: 1\r\n\r\n", "0\r\ntRaNsFeR-EnCoDiNg: chunked\r\n\r\n",
                 "0\r\nHost: other\r\n\r\n", "0\r\nAuthorization: value\r\n\r\n");
    sparenode::http::HttpBodyDecoder decoder({.chunked = true}, {});
    std::array<std::byte, 64> output{};
    auto result = decoder.feed(bytes(wire), output);
    REQUIRE_FALSE(result.has_value());
    const auto again = decoder.feed(bytes("0\r\n\r\n"), output);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code == result.error().code);
    CHECK_FALSE(decoder.complete());
}

TEST_CASE("Chunk decoder rejects EOF at every unfinished wire boundary", "[http][chunked][eof]")
{
    const std::string wire = "1\r\nx\r\n0\r\nX-End: yes\r\n\r\n";
    for (std::size_t length = 0; length < wire.size(); ++length)
    {
        sparenode::http::HttpBodyDecoder decoder({.chunked = true}, {});
        std::array<std::byte, 32> output{};
        REQUIRE(decoder.feed(bytes(wire).first(length), output).has_value());
        const auto result = decoder.finish();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == sparenode::http::HttpRequestParseErrorCode::incomplete_body);
    }
}

TEST_CASE("Chunk decoder enforces independent payload and metadata bounds",
          "[http][chunked][limits]")
{
    using Code = sparenode::http::HttpRequestParseErrorCode;
    sparenode::http::HttpRequestParserLimits limits;
    std::string wire;
    Code expected{};
    SECTION("cumulative decoded payload")
    {
        limits.max_body_bytes = 1;
        wire = "1\r\nx\r\n1\r\ny\r\n0\r\n\r\n";
        expected = Code::body_too_large;
    }
    SECTION("size line")
    {
        limits.max_chunk_line_bytes = 2;
        wire = "000\r\n\r\n";
        expected = Code::chunk_metadata_too_large;
    }
    SECTION("aggregate framing")
    {
        limits.max_chunk_metadata_bytes = 9;
        wire = "1\r\nx\r\n0\r\n\r\n";
        expected = Code::chunk_metadata_too_large;
    }
    SECTION("trailer bytes")
    {
        limits.max_trailer_bytes = 1;
        wire = "0\r\n\r\n";
        expected = Code::trailers_too_large;
    }
    SECTION("trailer count")
    {
        limits.max_trailer_count = 0;
        wire = "0\r\nX: y\r\n\r\n";
        expected = Code::trailers_too_large;
    }
    sparenode::http::HttpBodyDecoder decoder({.chunked = true}, limits);
    std::array<std::byte, 64> output{};
    const auto result = decoder.feed(bytes(wire), output);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == expected);
}

TEST_CASE("Chunk decoder accepts exact bounds and output backpressure", "[http][chunked][limits]")
{
    const sparenode::http::HttpRequestParserLimits limits{.max_body_bytes = 1,
                                                          .max_chunk_line_bytes = 1,
                                                          .max_chunk_metadata_bytes = 10,
                                                          .max_trailer_bytes = 2,
                                                          .max_trailer_count = 0};
    sparenode::http::HttpBodyDecoder decoder({.chunked = true}, limits);
    const auto first = decoder.feed(bytes("1\r\nx\r\n0\r\n\r\n"), {});
    REQUIRE(first.has_value());
    CHECK(first->consumed == 3);
    CHECK(first->produced == 0);
    std::array<std::byte, 1> output{};
    const auto rest = decoder.feed(bytes("x\r\n0\r\n\r\nNEXT"), output);
    REQUIRE(rest.has_value());
    CHECK(rest->consumed == 8);
    CHECK(rest->produced == 1);
    CHECK(rest->complete);
    CHECK(text(output) == "x");
}

TEST_CASE("Fixed bodies share the streaming decoder and enforce EOF", "[http][body][eof]")
{
    sparenode::http::HttpBodyDecoder decoder({.content_length = 3}, {});
    std::array<std::byte, 2> output{};
    auto first = decoder.feed(bytes("abcNEXT"), output);
    REQUIRE(first.has_value());
    CHECK(first->produced == 2);
    CHECK_FALSE(decoder.complete());
    auto last = decoder.feed(bytes("cNEXT"), output);
    REQUIRE(last.has_value());
    CHECK(last->consumed == 1);
    CHECK(last->complete);
    sparenode::http::HttpBodyDecoder truncated({.content_length = 3}, {});
    REQUIRE(truncated.feed(bytes("a"), output).has_value());
    CHECK_FALSE(truncated.finish().has_value());
    CHECK_FALSE(truncated.feed(bytes("bc"), output).has_value());
}

TEST_CASE("Request framing rejects smuggling ambiguities before body ingestion",
          "[http][chunked][security]")
{
    const std::string fields = GENERATE(
        "Transfer-Encoding: chunked\r\nContent-Length: 0\r\n",
        "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n",
        "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n",
        "Transfer-Encoding: chunked, chunked\r\n", "Transfer-Encoding: chunked, gzip\r\n",
        "Transfer-Encoding: gzip, chunked\r\n", "Transfer-Encoding: chunked;foo=bar\r\n",
        "Transfer-Encoding: \r\n", "Transfer-Encoding: ,chunked\r\n",
        "Transfer-Encoding : chunked\r\n", "Transfer-Encoding: chunked\r\n Content-Length: 0\r\n");
    const auto result = sparenode::http::parse_http_request_head(
        bytes("POST / HTTP/1.1\r\nHost: local\r\n" + fields + "\r\n"));
    CHECK_FALSE(result.has_value());
}

TEST_CASE("Chunked request parser returns owned decoded payload and wire length",
          "[http][chunked][parser]")
{
    const std::string source = std::string(head) + std::string("3\r\na\0b\r\n0\r\n\r\n", 13);
    // Keep header backing storage stable while exercising decoded-body copy ownership.
    const std::string stable = source + "NEXT";
    auto parsed = sparenode::http::parse_http_request(bytes(stable));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->is_complete());
    CHECK(parsed->consumed_bytes() == source.size());
    auto request = sparenode::test::require_optional(parsed->request());
    parsed = sparenode::http::HttpRequestParseResult::incomplete();
    CHECK(request.body().size() == 3);
    CHECK(request.body()[1] == std::byte{0});
    CHECK(request.header("Transfer-Encoding") == "chunked");
}

TEST_CASE("Buffered request feeds one-byte fragments without retaining chunk framing",
          "[http][chunked][session]")
{
    const std::string source = std::string(head) + "1\r\na\r\n1\r\nb\r\n0\r\nX: ignored\r\n\r\n";
    sparenode::http::detail::BufferedRequest request({});
    for (const auto &byte : source)
    {
        REQUIRE(request.feed(bytes(std::string_view(&byte, 1))).has_value());
    }
    REQUIRE(request.complete());
    const auto view = request.request();
    CHECK(text(view.body()) == "ab");
    CHECK(view.header("X").empty());
}
