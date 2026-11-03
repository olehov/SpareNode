#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>

#include "sparenode/filesystem/safe_path.hpp"
#include "sparenode/filesystem/shared_root.hpp"
#include "sparenode/foundation/result.hpp"

namespace sparenode::application
{
namespace detail
{
struct FilesystemResourceCoordinatorState;
}

/// @brief Selects compatible shared reading or exclusive resource mutation.
enum class FilesystemResourceAccessMode : std::uint8_t
{
    shared_read,       ///< May overlap other reads of the same resource hierarchy.
    exclusive_mutation ///< Conflicts with reads and mutations of ancestors or descendants.
};

/// @brief Describes one resource claim in a deterministic multi-resource acquisition.
struct FilesystemResourceAccessRequest
{
    const filesystem::SharedRoot *root{}; ///< Configured root forming part of the identity.
    std::string_view requested_path;      ///< Untrusted root-relative request path.
    FilesystemResourceAccessMode mode{};  ///< Compatibility required by the operation.
};

/// @brief Carries cancellation and an absolute deadline into resource acquisition.
struct FilesystemResourceAccessOptions
{
    std::stop_token stop_token{}; ///< Cancellation checked before ownership is granted.
    std::optional<std::chrono::steady_clock::time_point> deadline{}; ///< Optional expiry.
};

/// @brief Identifies why coordinated ownership could not be granted.
enum class FilesystemResourceAccessErrorCode : std::uint8_t
{
    invalid_path,               ///< SafePath rejected the requested resource identity.
    conflict,                   ///< An incompatible active claim overlaps the resource.
    cancelled,                  ///< Cancellation was requested before acquisition.
    deadline_exceeded,          ///< The acquisition deadline elapsed before ownership.
    capacity_exceeded,          ///< The bounded active-claim registry is full.
    resource_allocation_failed, ///< Identity or registry allocation failed.
};

/// @brief Preserves a transport-neutral coordination failure and optional path detail.
struct FilesystemResourceAccessError
{
    FilesystemResourceAccessErrorCode code{
        FilesystemResourceAccessErrorCode::invalid_path};  ///< Stable application failure category.
    std::optional<filesystem::SafePathError> path_error{}; ///< SafePath detail when invalid.
};

/// @brief Releases one coordinated claim set when moved-from or destroyed.
class FilesystemResourceLease final
{
  public:
    FilesystemResourceLease(const FilesystemResourceLease &) = delete;
    FilesystemResourceLease &operator=(const FilesystemResourceLease &) = delete;
    /// @brief Transfers registered claim ownership from another lease.
    /// @param[in,out] other Lease left without an active claim set.
    FilesystemResourceLease(FilesystemResourceLease &&other) noexcept;
    /// @brief Releases current claims and takes ownership from another lease.
    /// @param[in,out] other Lease left without an active claim set.
    /// @return This lease after the ownership transfer.
    FilesystemResourceLease &operator=(FilesystemResourceLease &&other) noexcept;
    ~FilesystemResourceLease();

  private:
    friend class FilesystemResourceCoordinator;

    /// @brief Adopts one registered claim set.
    /// @param[in] state Registry retaining the active claim set.
    /// @param[in] identifier Nonzero claim-set identifier to release.
    FilesystemResourceLease(std::shared_ptr<detail::FilesystemResourceCoordinatorState> state,
                            std::uint64_t identifier) noexcept;
    /// @brief Removes the currently owned claim set, if any.
    void release() noexcept;

    std::shared_ptr<detail::FilesystemResourceCoordinatorState> state_; ///< Shared registry state.
    std::uint64_t identifier_{}; ///< Nonzero active claim-set identifier.
};

/// @brief Coordinates in-process access to bounded root-relative filesystem resources.
///
/// Conflicting requests are rejected immediately rather than queued. This keeps acquisition
/// bounded by the caller's active policy and avoids lock-wait deadlocks. Native descriptor-based
/// filesystem containment remains authoritative after a lease is granted.
class FilesystemResourceCoordinator final
{
  public:
    /// @brief Maximum simultaneously retained claims across all active operations.
    static constexpr std::size_t maximum_active_claims = 4096;

    /// @brief Creates an empty bounded coordination registry.
    FilesystemResourceCoordinator();

    /// @brief Acquires one shared or exclusive resource claim without waiting.
    /// @param[in] root Configured filesystem root forming part of the identity.
    /// @param[in] requested_path Untrusted root-relative resource path.
    /// @param[in] mode Shared-read or exclusive-mutation compatibility.
    /// @param[in] options Cancellation and expiry checked before ownership is granted.
    /// @return RAII lease, or a stable failure without retaining a claim.
    [[nodiscard]] Result<FilesystemResourceLease, FilesystemResourceAccessError>
    acquire(const filesystem::SharedRoot &root, std::string_view requested_path,
            FilesystemResourceAccessMode mode,
            const FilesystemResourceAccessOptions &options = {}) const;

    /// @brief Acquires an ordered claim set atomically without partial ownership.
    /// @param[in] requests Resources normalized through SafePath before registry access.
    /// @param[in] options Cancellation and expiry checked before ownership is granted.
    /// @return RAII lease, or a stable failure without retaining any claim.
    [[nodiscard]] Result<FilesystemResourceLease, FilesystemResourceAccessError>
    acquire_all(std::span<const FilesystemResourceAccessRequest> requests,
                const FilesystemResourceAccessOptions &options = {}) const;

  private:
    std::shared_ptr<detail::FilesystemResourceCoordinatorState> state_; ///< Shared active claims.
};

} // namespace sparenode::application
