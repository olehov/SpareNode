#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <stop_token>
#include <string>
#include <vector>

#include "sparenode/application/filesystem_resource_coordinator.hpp"
#include "sparenode/filesystem/shared_root.hpp"
#include "support/temporary_directory.hpp"

namespace
{

/// @brief Creates one validated root for coordinator tests.
[[nodiscard]] sparenode::filesystem::SharedRoot
make_root(const sparenode::test::TemporaryDirectory &directory)
{
    auto root = sparenode::filesystem::SharedRoot::create(directory.path());
    REQUIRE(root);
    return std::move(root).value();
}

} // namespace

TEST_CASE("Resource coordinator permits shared reads and rejects an overlapping mutation",
          "[application][filesystem-coordinator][conflict]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-coordinator-shared");
    const auto root = make_root(directory);
    const sparenode::application::FilesystemResourceCoordinator coordinator;

    auto first = coordinator.acquire(
        root, "folder/file.txt", sparenode::application::FilesystemResourceAccessMode::shared_read);
    REQUIRE(first);
    auto second = coordinator.acquire(
        root, "folder/file.txt", sparenode::application::FilesystemResourceAccessMode::shared_read);
    REQUIRE(second);

    const auto mutation = coordinator.acquire(
        root, "folder/file.txt",
        sparenode::application::FilesystemResourceAccessMode::exclusive_mutation);
    REQUIRE_FALSE(mutation);
    CHECK(mutation.error().code ==
          sparenode::application::FilesystemResourceAccessErrorCode::conflict);
}

TEST_CASE("Resource coordinator detects ancestor conflicts but permits unrelated resources",
          "[application][filesystem-coordinator][hierarchy]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-coordinator-hierarchy");
    const auto root = make_root(directory);
    const sparenode::application::FilesystemResourceCoordinator coordinator;

    auto descendant =
        coordinator.acquire(root, "directory/child/file.txt",
                            sparenode::application::FilesystemResourceAccessMode::shared_read);
    REQUIRE(descendant);

    const auto ancestor = coordinator.acquire(
        root, "directory",
        sparenode::application::FilesystemResourceAccessMode::exclusive_mutation);
    REQUIRE_FALSE(ancestor);
    CHECK(ancestor.error().code ==
          sparenode::application::FilesystemResourceAccessErrorCode::conflict);

    CHECK(coordinator.acquire(
        root, "unrelated",
        sparenode::application::FilesystemResourceAccessMode::exclusive_mutation));

    auto mutation = coordinator.acquire(
        root, "mutation", sparenode::application::FilesystemResourceAccessMode::exclusive_mutation);
    REQUIRE(mutation);
    const auto duplicate_mutation = coordinator.acquire(
        root, "mutation", sparenode::application::FilesystemResourceAccessMode::exclusive_mutation);
    REQUIRE_FALSE(duplicate_mutation);
    CHECK(duplicate_mutation.error().code ==
          sparenode::application::FilesystemResourceAccessErrorCode::conflict);
}

TEST_CASE("Resource coordinator releases ownership through every moved RAII lease",
          "[application][filesystem-coordinator][raii]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-coordinator-raii");
    const auto root = make_root(directory);
    const sparenode::application::FilesystemResourceCoordinator coordinator;

    {
        auto acquired = coordinator.acquire(
            root, "file.txt",
            sparenode::application::FilesystemResourceAccessMode::exclusive_mutation);
        REQUIRE(acquired);
        auto moved = std::move(acquired).value();
        const auto conflict = coordinator.acquire(
            root, "file.txt", sparenode::application::FilesystemResourceAccessMode::shared_read);
        REQUIRE_FALSE(conflict);
    }

    CHECK(coordinator.acquire(root, "file.txt",
                              sparenode::application::FilesystemResourceAccessMode::shared_read));
}

TEST_CASE("Resource coordinator observes cancellation and deadlines without retaining claims",
          "[application][filesystem-coordinator][interruption]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-coordinator-interruption");
    const auto root = make_root(directory);
    const sparenode::application::FilesystemResourceCoordinator coordinator;
    std::stop_source cancelled;
    cancelled.request_stop();

    const auto cancellation = coordinator.acquire(
        root, "file.txt", sparenode::application::FilesystemResourceAccessMode::shared_read,
        {cancelled.get_token(), {}});
    REQUIRE_FALSE(cancellation);
    CHECK(cancellation.error().code ==
          sparenode::application::FilesystemResourceAccessErrorCode::cancelled);

    const auto expiry = coordinator.acquire(
        root, "file.txt", sparenode::application::FilesystemResourceAccessMode::shared_read,
        {{}, std::chrono::steady_clock::time_point::min()});
    REQUIRE_FALSE(expiry);
    CHECK(expiry.error().code ==
          sparenode::application::FilesystemResourceAccessErrorCode::deadline_exceeded);

    CHECK(coordinator.acquire(root, "file.txt",
                              sparenode::application::FilesystemResourceAccessMode::shared_read));
}

TEST_CASE("Multi-resource acquisition is atomic when any requested resource conflicts",
          "[application][filesystem-coordinator][multi-resource]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-coordinator-multi");
    const auto root = make_root(directory);
    const sparenode::application::FilesystemResourceCoordinator coordinator;
    auto occupied = coordinator.acquire(
        root, "occupied", sparenode::application::FilesystemResourceAccessMode::shared_read);
    REQUIRE(occupied);

    const std::array requests{
        sparenode::application::FilesystemResourceAccessRequest{
            &root, "free",
            sparenode::application::FilesystemResourceAccessMode::exclusive_mutation},
        sparenode::application::FilesystemResourceAccessRequest{
            &root, "occupied",
            sparenode::application::FilesystemResourceAccessMode::exclusive_mutation}};
    const auto combined = coordinator.acquire_all(requests);
    REQUIRE_FALSE(combined);
    CHECK(combined.error().code ==
          sparenode::application::FilesystemResourceAccessErrorCode::conflict);

    CHECK(coordinator.acquire(
        root, "free", sparenode::application::FilesystemResourceAccessMode::exclusive_mutation));
}

TEST_CASE("Resource coordinator bounds and reclaims its active claim table",
          "[application][filesystem-coordinator][capacity]")
{
    const sparenode::test::TemporaryDirectory directory("sparenode-coordinator-capacity");
    const auto root = make_root(directory);
    const sparenode::application::FilesystemResourceCoordinator coordinator;
    constexpr auto capacity =
        sparenode::application::FilesystemResourceCoordinator::maximum_active_claims;
    std::vector<std::string> paths;
    std::vector<sparenode::application::FilesystemResourceAccessRequest> requests;
    paths.reserve(capacity);
    requests.reserve(capacity);
    for (std::size_t index = 0; index < capacity; ++index)
    {
        paths.push_back("resource-" + std::to_string(index));
        requests.push_back({&root, paths.back(),
                            sparenode::application::FilesystemResourceAccessMode::shared_read});
    }

    {
        auto full = coordinator.acquire_all(requests);
        REQUIRE(full);
        const auto overflow = coordinator.acquire(
            root, "overflow", sparenode::application::FilesystemResourceAccessMode::shared_read);
        REQUIRE_FALSE(overflow);
        CHECK(overflow.error().code ==
              sparenode::application::FilesystemResourceAccessErrorCode::capacity_exceeded);
    }

    CHECK(coordinator.acquire(root, "after-release",
                              sparenode::application::FilesystemResourceAccessMode::shared_read));
}
