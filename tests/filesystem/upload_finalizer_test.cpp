#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include "sparenode/configuration/shared_root.hpp"
#include "sparenode/filesystem/temporary_file.hpp"
#include "sparenode/filesystem/upload_finalizer.hpp"
#include "support/temporary_directory.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{

/// @brief Borrows fixture text as immutable file bytes.
[[nodiscard]] std::span<const std::byte> bytes(const std::string_view text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

/// @brief Creates a complete artifact using the same lifecycle as request ingestion.
[[nodiscard]] sparenode::filesystem::TemporaryFile complete_source(const std::string_view text)
{
    auto created = sparenode::filesystem::TemporaryFile::create();
    REQUIRE(created.has_value());
    auto file = std::move(created).value();
    REQUIRE(file.write(bytes(text)).has_value());
    REQUIRE(file.complete().has_value());
    return file;
}

/// @brief Reads an exactly published destination without changing its ownership.
[[nodiscard]] std::string read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// @brief Verifies that failed publication did not leave a visible sibling stage.
[[nodiscard]] bool has_stage(const std::filesystem::path &directory)
{
    const auto entries = std::filesystem::directory_iterator(directory);
    return std::any_of(
        begin(entries), end(entries), [](const auto &entry)
        { return entry.path().filename().string().starts_with(".sparenode-upload-"); });
}

/// @brief Owns one isolated share and its validated root for finalization tests.
struct UploadFixture
{
    sparenode::test::TemporaryDirectory temporary{"sparenode-upload-finalizer"}; ///< Test tree.
    std::filesystem::path share{temporary.path() / "share"}; ///< Root directory.
    sparenode::configuration::SharedRoot root;               ///< Validated security boundary.

    /// @brief Creates the share before validation.
    UploadFixture()
        : root(
              [this]
              {
                  std::filesystem::create_directory(share);
                  auto created = sparenode::configuration::SharedRoot::create(share);
                  REQUIRE(created.has_value());
                  return std::move(created).value();
              }())
    {
    }
};

/// @brief Requests cancellation after the source is validated and staging is created.
class CancelAfterStageSource final : public sparenode::filesystem::TemporaryFile
{
  public:
    CancelAfterStageSource(sparenode::filesystem::TemporaryFile source, std::stop_source &stop)
        : source_(std::move(source)), stop_(stop)
    {
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept override
    {
        return source_.path();
    }

    [[nodiscard]] std::uint64_t size() const noexcept override
    {
        if (++size_queries_ == 2)
        {
            stop_.request_stop();
        }
        return source_.size();
    }

    [[nodiscard]] bool completed() const noexcept override
    {
        return source_.completed();
    }

  private:
    sparenode::filesystem::TemporaryFile source_; ///< Owns cleanup of the actual artifact.
    std::stop_source &stop_;                      ///< Requests cancellation after staging.
    mutable std::size_t size_queries_{};          ///< Distinguishes validation from copying.
};

} // namespace

TEST_CASE("Completed upload is published under its final name without retaining staging",
          "[filesystem][upload]")
{
    UploadFixture fixture;
    std::filesystem::create_directory(fixture.share / "nested");
    const std::string payload(200000, 'x');
    std::filesystem::path source_path;
    {
        auto source = complete_source(payload);
        source_path = source.path();
        const auto published =
            sparenode::filesystem::finalize_upload(fixture.root, "nested/file.bin", source);
        REQUIRE(published.has_value());
        CHECK(
            std::filesystem::equivalent(published.value(), fixture.share / "nested" / "file.bin"));
        CHECK(read_file(published.value()) == payload);
        CHECK((has_stage(fixture.share / "nested")) == false);
        CHECK(std::filesystem::exists(source_path));
    }
    CHECK((std::filesystem::exists(source_path)) == false);
    CHECK(read_file(fixture.share / "nested" / "file.bin") == payload);
#ifdef _WIN32
    const auto attributes = GetFileAttributesW((fixture.share / "nested" / "file.bin").c_str());
    REQUIRE(attributes != INVALID_FILE_ATTRIBUTES);
    CHECK((attributes & FILE_ATTRIBUTE_HIDDEN) == 0);
#endif
}

TEST_CASE("Finalization preserves an existing destination and cleans its sibling stage",
          "[filesystem][upload][security]")
{
    UploadFixture fixture;
    const auto destination = fixture.share / "existing.txt";
    {
        std::ofstream output(destination, std::ios::binary);
        output << "original";
    }
    auto source = complete_source("replacement");
    const auto result =
        sparenode::filesystem::finalize_upload(fixture.root, "existing.txt", source);
    REQUIRE((result.has_value()) == false);
    CHECK(result.error().code ==
          sparenode::filesystem::UploadFinalizationErrorCode::destination_exists);
    CHECK(read_file(destination) == "original");
    CHECK((has_stage(fixture.share)) == false);
}

TEST_CASE("Incomplete, cancelled, and expired uploads never receive a final name",
          "[filesystem][upload][cancel][timeout]")
{
    UploadFixture fixture;
    auto created = sparenode::filesystem::TemporaryFile::create();
    REQUIRE(created.has_value());
    auto partial = std::move(created).value();
    REQUIRE(partial.write(bytes("partial")).has_value());
    const auto incomplete =
        sparenode::filesystem::finalize_upload(fixture.root, "file.txt", partial);
    REQUIRE((incomplete.has_value()) == false);
    CHECK(incomplete.error().code ==
          sparenode::filesystem::UploadFinalizationErrorCode::invalid_source);
    auto complete = complete_source("complete");
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = sparenode::filesystem::finalize_upload(
        fixture.root, "file.txt", complete, {.stop_token = stop.get_token()});
    REQUIRE((cancelled.has_value()) == false);
    CHECK(cancelled.error().code == sparenode::filesystem::UploadFinalizationErrorCode::cancelled);
    const auto expired = sparenode::filesystem::finalize_upload(
        fixture.root, "file.txt", complete,
        {.deadline = std::chrono::steady_clock::now() - std::chrono::seconds{1}});
    REQUIRE((expired.has_value()) == false);
    CHECK(expired.error().code ==
          sparenode::filesystem::UploadFinalizationErrorCode::deadline_exceeded);
    CHECK((std::filesystem::exists(fixture.share / "file.txt")) == false);
    CHECK((has_stage(fixture.share)) == false);
}

TEST_CASE("Cancellation after staging removes the sibling and preserves the final name",
          "[filesystem][upload][cancel]")
{
    UploadFixture fixture;
    std::stop_source stop;
    CancelAfterStageSource source(complete_source("interrupted"), stop);
    const auto result = sparenode::filesystem::finalize_upload(
        fixture.root, "interrupted.txt", source, {.stop_token = stop.get_token()});
    REQUIRE((result.has_value()) == false);
    CHECK(result.error().code == sparenode::filesystem::UploadFinalizationErrorCode::cancelled);
    CHECK(stop.stop_requested());
    CHECK(std::filesystem::exists(fixture.share / "interrupted.txt") == false);
    CHECK(has_stage(fixture.share) == false);
}

TEST_CASE("Finalization rejects escapes and unavailable parents", "[filesystem][upload][security]")
{
    UploadFixture fixture;
    auto source = complete_source("safe");
    for (const std::string_view path : {"", "../outside.txt", "/absolute.txt"})
    {
        const auto result = sparenode::filesystem::finalize_upload(fixture.root, path, source);
        REQUIRE((result.has_value()) == false);
        CHECK(result.error().code ==
              sparenode::filesystem::UploadFinalizationErrorCode::invalid_destination);
    }
    const auto missing =
        sparenode::filesystem::finalize_upload(fixture.root, "missing/file.txt", source);
    REQUIRE((missing.has_value()) == false);
    CHECK(missing.error().code ==
          sparenode::filesystem::UploadFinalizationErrorCode::parent_unavailable);
    CHECK((has_stage(fixture.share)) == false);
}

TEST_CASE("Finalization cannot publish through an external directory link",
          "[filesystem][upload][security]")
{
    UploadFixture fixture;
    const auto outside = fixture.temporary.path() / "outside";
    std::filesystem::create_directory(outside);
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, fixture.share / "external", error);
    if (error)
    {
#ifdef _WIN32
        SKIP("Windows symbolic-link creation requires Developer Mode or the symlink privilege");
#else
        FAIL("Cannot create the external directory link: " << error.message());
#endif
    }
    auto source = complete_source("private");
    const auto result =
        sparenode::filesystem::finalize_upload(fixture.root, "external/file.txt", source);
    REQUIRE((result.has_value()) == false);
    CHECK(std::filesystem::exists(outside / "file.txt") == false);
    CHECK(has_stage(outside) == false);
}
