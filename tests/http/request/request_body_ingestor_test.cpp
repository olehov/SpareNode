#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <span>
#include <stop_token>
#include <string>
#include <utility>

#include "sparenode/http/request/request_body_ingestor.hpp"

namespace
{
/// @brief Produces one controlled native-I/O failure for ingestion mapping tests.
class FailingTemporaryFile final : public sparenode::filesystem::TemporaryFile
{
  public:
    explicit FailingTemporaryFile(const sparenode::filesystem::TemporaryFileErrorCode code)
        : code_(code)
    {
    }

    [[nodiscard]] sparenode::Result<void, sparenode::filesystem::TemporaryFileError>
    write(std::span<const std::byte>) override
    {
        if (code_ == sparenode::filesystem::TemporaryFileErrorCode::write_failed)
        {
            return sparenode::unexpected(
                sparenode::filesystem::TemporaryFileError{code_, native_error});
        }
        return {};
    }

    [[nodiscard]] sparenode::Result<void, sparenode::filesystem::TemporaryFileError>
    complete() override
    {
        return sparenode::unexpected(
            sparenode::filesystem::TemporaryFileError{code_, native_error});
    }

  private:
    static constexpr int native_error = 1234;
    sparenode::filesystem::TemporaryFileErrorCode code_;
};

/// @brief Borrows string fixture storage as immutable bytes.
[[nodiscard]] std::span<const std::byte> bytes(const std::string_view text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

/// @brief Reads the exact completed body owned by one ingestion artifact.
[[nodiscard]] std::string read_body(const sparenode::http::RequestBodyIngestor &ingestor)
{
    const auto artifact = ingestor.artifact();
    REQUIRE(artifact);
    REQUIRE(artifact->completed());
    std::ifstream input(artifact->path(), std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace

TEST_CASE("Request body ingestor streams a fixed body larger than its decode buffer",
          "[http][body][temporary-file]")
{
    const std::string payload(20000, 'x');
    sparenode::http::RequestBodyIngestor ingestor({.content_length = payload.size()}, {});
    const auto progress = ingestor.feed(bytes(payload + "NEXT"));
    REQUIRE(progress.has_value());
    CHECK(progress->consumed == payload.size());
    CHECK(progress->produced == payload.size());
    REQUIRE(ingestor.complete());
    CHECK(ingestor.body_size() == payload.size());
    CHECK(read_body(ingestor) == payload);
}

TEST_CASE("Request body ingestor sends decoded chunk payload through the same file sink",
          "[http][body][chunked][temporary-file]")
{
    constexpr std::string_view wire = "4\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Ignored: yes\r\n\r\nNEXT";
    constexpr std::size_t framing_bytes = wire.size() - 4;
    sparenode::http::RequestBodyIngestor ingestor({.chunked = true}, {});
    const auto progress = ingestor.feed(bytes(wire));
    REQUIRE(progress.has_value());
    CHECK(progress->consumed == framing_bytes);
    CHECK(progress->produced == 9);
    REQUIRE(ingestor.complete());
    CHECK(ingestor.body_size() == 9);
    CHECK(read_body(ingestor) == "Wikipedia");
}

TEST_CASE("Request body ingestor completes an empty body without creating a file",
          "[http][body][temporary-file]")
{
    sparenode::http::RequestBodyIngestor ingestor({.content_length = 0}, {});
    CHECK(ingestor.complete());
    CHECK(ingestor.body_size() == 0);
    CHECK_FALSE(ingestor.artifact());
    REQUIRE(ingestor.finish().has_value());
}

TEST_CASE("Request body ingestor rejects fixed and decoded chunked bodies above the limit",
          "[http][body][limits][temporary-file]")
{
    SECTION("fixed length is rejected before payload bytes")
    {
        sparenode::http::HttpRequestParserLimits limits{};
        limits.max_body_bytes = 3;
        sparenode::http::RequestBodyIngestor ingestor({.content_length = 4}, limits);
        const auto result = ingestor.feed({});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code ==
              sparenode::http::RequestBodyIngestionErrorCode::body_too_large);
        CHECK_FALSE(ingestor.artifact());
    }

    SECTION("chunked payload is bounded after decoding")
    {
        sparenode::http::HttpRequestParserLimits limits{};
        limits.max_body_bytes = 3;
        sparenode::http::RequestBodyIngestor ingestor({.chunked = true}, limits);
        const auto result = ingestor.feed(bytes("4\r\ndata\r\n0\r\n\r\n"));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code ==
              sparenode::http::RequestBodyIngestionErrorCode::body_too_large);
    }
}

TEST_CASE("Request body ingestor preserves premature EOF and cancellation failures",
          "[http][body][error][temporary-file]")
{
    SECTION("premature EOF remains a framing failure")
    {
        sparenode::http::RequestBodyIngestor ingestor({.content_length = 4}, {});
        REQUIRE(ingestor.feed(bytes("ab")).has_value());
        const auto failure = ingestor.finish();
        REQUIRE_FALSE(failure.has_value());
        CHECK(failure.error().code ==
              sparenode::http::RequestBodyIngestionErrorCode::framing_failure);
        const auto repeated = ingestor.finish();
        REQUIRE_FALSE(repeated.has_value());
        CHECK(repeated.error().code == failure.error().code);
        CHECK(repeated.error().framing_error.code == failure.error().framing_error.code);
        CHECK_FALSE(ingestor.artifact());
    }

    SECTION("cancellation remains distinguishable")
    {
        sparenode::http::RequestBodyIngestor ingestor({.content_length = 4}, {});
        REQUIRE(ingestor.feed(bytes("ab")).has_value());
        std::stop_source stop;
        stop.request_stop();
        const auto failure = ingestor.feed(bytes("cd"), stop.get_token());
        REQUIRE_FALSE(failure.has_value());
        CHECK(failure.error().code == sparenode::http::RequestBodyIngestionErrorCode::cancelled);
        const auto repeated = ingestor.feed(bytes("cd"));
        REQUIRE_FALSE(repeated.has_value());
        CHECK(repeated.error().code == failure.error().code);
        CHECK_FALSE(ingestor.artifact());
    }
}

TEST_CASE("Request body ingestor propagates temporary file lifecycle failures",
          "[http][body][error][temporary-file]")
{
    using sparenode::filesystem::TemporaryFile;
    using sparenode::filesystem::TemporaryFileError;
    using sparenode::filesystem::TemporaryFileErrorCode;

    SECTION("creation")
    {
        sparenode::http::RequestBodyIngestor ingestor(
            {.content_length = 1}, {},
            []() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError> {
                return sparenode::unexpected(
                    TemporaryFileError{TemporaryFileErrorCode::create_failed, 1234});
            });
        const auto result = ingestor.feed(bytes("x"));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code ==
              sparenode::http::RequestBodyIngestionErrorCode::temporary_file_failure);
        CHECK(result.error().file_error.code == TemporaryFileErrorCode::create_failed);
        CHECK(result.error().file_error.native_code == 1234);
    }

    SECTION("write")
    {
        sparenode::http::RequestBodyIngestor ingestor(
            {.content_length = 1}, {},
            []() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError>
            {
                return std::static_pointer_cast<TemporaryFile>(
                    std::make_shared<FailingTemporaryFile>(TemporaryFileErrorCode::write_failed));
            });
        const auto result = ingestor.feed(bytes("x"));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code ==
              sparenode::http::RequestBodyIngestionErrorCode::temporary_file_failure);
        CHECK(result.error().file_error.code == TemporaryFileErrorCode::write_failed);
    }

    for (const auto code :
         {TemporaryFileErrorCode::flush_failed, TemporaryFileErrorCode::close_failed})
    {
        DYNAMIC_SECTION("completion failure " << static_cast<int>(code))
        {
            sparenode::http::RequestBodyIngestor ingestor(
                {.content_length = 1}, {},
                [code]() -> sparenode::Result<std::shared_ptr<TemporaryFile>, TemporaryFileError> {
                    return std::static_pointer_cast<TemporaryFile>(
                        std::make_shared<FailingTemporaryFile>(code));
                });
            const auto result = ingestor.feed(bytes("x"));
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code ==
                  sparenode::http::RequestBodyIngestionErrorCode::temporary_file_failure);
            CHECK(result.error().file_error.code == code);
        }
    }
}

TEST_CASE("Cancelled ingestion destroys and removes its partial native artifact",
          "[http][body][cancel][temporary-file]")
{
    std::filesystem::path path;
    std::weak_ptr<sparenode::filesystem::TemporaryFile> observer;
    {
        sparenode::http::RequestBodyIngestor ingestor(
            {.content_length = 4}, {},
            [&]() -> sparenode::Result<std::shared_ptr<sparenode::filesystem::TemporaryFile>,
                                       sparenode::filesystem::TemporaryFileError>
            {
                auto created = sparenode::filesystem::TemporaryFile::create();
                if (!created)
                {
                    return sparenode::unexpected(created.error());
                }
                auto artifact = std::make_shared<sparenode::filesystem::TemporaryFile>(
                    std::move(created).value());
                path = artifact->path();
                observer = artifact;
                return artifact;
            });
        REQUIRE(ingestor.feed(bytes("ab")).has_value());
        REQUIRE(std::filesystem::exists(path));
        std::stop_source stop;
        stop.request_stop();
        const auto result = ingestor.feed(bytes("cd"), stop.get_token());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == sparenode::http::RequestBodyIngestionErrorCode::cancelled);
        CHECK(observer.expired());
        CHECK_FALSE(std::filesystem::exists(path));
    }
    CHECK(observer.expired());
    CHECK_FALSE(std::filesystem::exists(path));
}
