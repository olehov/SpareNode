#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>

#include "sparenode/application/filesystem_service.hpp"
#include "sparenode/filesystem/location_permissions.hpp"
#include "sparenode/filesystem/shared_root.hpp"
#include "support/temporary_directory.hpp"

namespace
{

/// @brief Exposes immutable text as bytes for upload tests.
[[nodiscard]] std::span<const std::byte> as_bytes(const std::string_view source)
{
    return std::as_bytes(std::span(source.data(), source.size()));
}

/// @brief Creates one application service rooted in a temporary directory.
[[nodiscard]] sparenode::application::FilesystemService
make_service(const sparenode::test::TemporaryDirectory &directory,
             const sparenode::filesystem::LocationPermissions permissions =
                 sparenode::filesystem::LocationPermissions())
{
    auto root = sparenode::filesystem::SharedRoot::create(directory.path());
    REQUIRE(root);
    return {std::move(root).value(), permissions};
}

/// @brief Creates one immutable file-backed upload artifact containing the supplied payload.
[[nodiscard]] std::shared_ptr<const sparenode::storage::TemporaryFile>
complete_artifact(const std::string_view payload)
{
    auto created = sparenode::storage::TemporaryFile::create();
    REQUIRE(created);
    auto artifact = std::move(created).value();
    REQUIRE(artifact.write(as_bytes(payload)));
    REQUIRE(artifact.complete());
    return std::make_shared<const sparenode::storage::TemporaryFile>(std::move(artifact));
}

} // namespace

TEST_CASE("Filesystem service resolves directory and file reads without HTTP",
          "[application][filesystem-service][read]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-service-read");
    REQUIRE(std::ofstream(directory.path() / "document.txt", std::ios::binary) << "content");
    const auto service = make_service(directory);

    auto listing_result = service.read({});
    REQUIRE(listing_result);
    const auto &listing =
        std::get<std::vector<sparenode::filesystem::DirectoryListingEntry>>(listing_result.value());
    CHECK(std::ranges::any_of(listing,
                              [](const auto &entry) { return entry.name == "document.txt"; }));

    auto file_result = service.read("document.txt");
    REQUIRE(file_result);
    auto resource = std::move(file_result).value();
    auto stream = std::get<sparenode::filesystem::FileReadStream>(std::move(resource));
    std::array<std::byte, 16> output{};
    const auto read = stream.read(output, std::stop_token{});
    REQUIRE(read);
    CHECK(std::string(reinterpret_cast<const char *>(output.data()), read.value()) == "content");
}

TEST_CASE("Filesystem service enforces each location permission before its operation",
          "[application][filesystem-service][permissions]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-service-permissions");
    const auto service =
        make_service(directory, sparenode::filesystem::LocationPermissions{false, false, false});
    constexpr std::string_view payload = "content";

    const auto read = service.read({});
    REQUIRE_FALSE(read);
    CHECK(read.error().code == sparenode::application::FilesystemReadErrorCode::forbidden);

    const auto upload = service.upload("file.txt", {as_bytes(payload), payload.size(), {}, {}});
    REQUIRE_FALSE(upload);
    CHECK(upload.error().code == sparenode::application::FilesystemUploadErrorCode::forbidden);

    const auto create = service.create_directory("directory", 1);
    REQUIRE_FALSE(create);
    CHECK(create.error().code ==
          sparenode::application::FilesystemDirectoryCreationErrorCode::forbidden);

    const auto remove = service.remove("missing", 1);
    REQUIRE_FALSE(remove);
    CHECK(remove.error().code == sparenode::application::FilesystemRemovalErrorCode::forbidden);
}

TEST_CASE("Filesystem service coordinates upload creation and deletion use cases",
          "[application][filesystem-service][write]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-service-write");
    const auto service =
        make_service(directory, sparenode::filesystem::LocationPermissions{true, true, true});
    constexpr std::string_view payload = "uploaded";

    const auto upload = service.upload("file.txt", {as_bytes(payload), payload.size(), {}, {}});
    REQUIRE(upload);
    CHECK(std::filesystem::file_size(directory.path() / "file.txt") == payload.size());

    const auto create = service.create_directory("directory", 0);
    REQUIRE(create);
    CHECK(std::filesystem::is_directory(directory.path() / "directory"));

    CHECK(service.remove("file.txt", 0));
    CHECK(service.remove("directory", 0));
    CHECK_FALSE(std::filesystem::exists(directory.path() / "file.txt"));
    CHECK_FALSE(std::filesystem::exists(directory.path() / "directory"));
}

TEST_CASE("Filesystem service rejects inconsistent or unexpected operation content",
          "[application][filesystem-service][validation]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-service-content");
    const auto service =
        make_service(directory, sparenode::filesystem::LocationPermissions{true, true, true});
    constexpr std::string_view payload = "content";

    const auto upload = service.upload("file.txt", {as_bytes(payload), payload.size() + 1, {}, {}});
    REQUIRE_FALSE(upload);
    CHECK(upload.error().code == sparenode::application::FilesystemUploadErrorCode::invalid_upload);

    const auto create = service.create_directory("directory", 1);
    REQUIRE_FALSE(create);
    CHECK(create.error().code ==
          sparenode::application::FilesystemDirectoryCreationErrorCode::content_not_allowed);

    const auto remove = service.remove("missing", 1);
    REQUIRE_FALSE(remove);
    CHECK(remove.error().code ==
          sparenode::application::FilesystemRemovalErrorCode::content_not_allowed);
}

TEST_CASE("Filesystem service publishes file-backed artifacts and preserves finalization errors",
          "[application][filesystem-service][upload]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-service-artifact");
    const auto service =
        make_service(directory, sparenode::filesystem::LocationPermissions{true, true, true});
    constexpr std::string_view payload = "artifact";

    const auto artifact = complete_artifact(payload);
    const auto published = service.upload("artifact.txt", {{}, artifact->size(), artifact, {}});
    REQUIRE(published);
    CHECK(std::filesystem::file_size(directory.path() / "artifact.txt") == payload.size());

    REQUIRE(std::ofstream(directory.path() / "existing.txt", std::ios::binary) << "original");
    const auto duplicate_artifact = complete_artifact(payload);
    const auto duplicate =
        service.upload("existing.txt", {{}, duplicate_artifact->size(), duplicate_artifact, {}});
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code ==
          sparenode::application::FilesystemUploadErrorCode::upload_finalization_failed);
    REQUIRE(std::holds_alternative<sparenode::filesystem::UploadFinalizationError>(
        duplicate.error().detail));
    CHECK(std::get<sparenode::filesystem::UploadFinalizationError>(duplicate.error().detail).code ==
          sparenode::filesystem::UploadFinalizationErrorCode::destination_exists);
}

TEST_CASE("Filesystem service retains lower-level details for failed confined operations",
          "[application][filesystem-service][errors]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-service-errors");
    const auto service =
        make_service(directory, sparenode::filesystem::LocationPermissions{true, true, true});

    const auto missing = service.read("missing");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code ==
          sparenode::application::FilesystemReadErrorCode::directory_listing_failed);
    REQUIRE(std::holds_alternative<sparenode::filesystem::DirectoryListingError>(
        missing.error().detail));
    CHECK(std::get<sparenode::filesystem::DirectoryListingError>(missing.error().detail).code ==
          sparenode::filesystem::DirectoryListingErrorCode::not_found);

    const auto upload_artifact = complete_artifact("content");
    const auto upload =
        service.upload("../outside", {{}, upload_artifact->size(), upload_artifact, {}});
    REQUIRE_FALSE(upload);
    CHECK(upload.error().code ==
          sparenode::application::FilesystemUploadErrorCode::upload_finalization_failed);
    CHECK(std::holds_alternative<sparenode::filesystem::UploadFinalizationError>(
        upload.error().detail));

    const auto create = service.create_directory("../outside", 0);
    REQUIRE_FALSE(create);
    CHECK(create.error().code ==
          sparenode::application::FilesystemDirectoryCreationErrorCode::directory_creation_failed);
    CHECK(std::holds_alternative<sparenode::filesystem::DirectoryCreationError>(
        create.error().detail));

    const auto remove = service.remove("../outside", 0);
    REQUIRE_FALSE(remove);
    CHECK(remove.error().code ==
          sparenode::application::FilesystemRemovalErrorCode::filesystem_removal_failed);
    CHECK(std::holds_alternative<sparenode::filesystem::FilesystemEntryRemovalError>(
        remove.error().detail));
}
