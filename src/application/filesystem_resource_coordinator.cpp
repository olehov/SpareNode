#include "sparenode/application/filesystem_resource_coordinator.hpp"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "sparenode/foundation/concurrency/interruption.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sparenode::application
{
namespace
{

/// @brief Stores one normalized root-relative identity and its compatibility mode.
struct ResourceClaim
{
    std::filesystem::path root;
    std::vector<std::filesystem::path> components;
    FilesystemResourceAccessMode mode{};
};

/// @brief Owns every claim registered by one atomic acquisition.
struct ActiveClaimSet
{
    std::uint64_t identifier{};
    std::vector<ResourceClaim> claims;
};

/// @brief Maps the shared interruption policy into the coordinator error domain.
[[nodiscard]] std::optional<FilesystemResourceAccessError>
interruption(const FilesystemResourceAccessOptions &options) noexcept
{
    using concurrency::InterruptionStatus;
    switch (concurrency::interruption_status(options.stop_token, options.deadline))
    {
    case InterruptionStatus::cancelled:
        return FilesystemResourceAccessError{FilesystemResourceAccessErrorCode::cancelled, {}};
    case InterruptionStatus::deadline_exceeded:
        return FilesystemResourceAccessError{FilesystemResourceAccessErrorCode::deadline_exceeded,
                                             {}};
    case InterruptionStatus::none:
        return std::nullopt;
    }
    return std::nullopt;
}

/// @brief Compares native path text according to the host filesystem naming policy.
[[nodiscard]] bool path_text_equal(const std::filesystem::path &left,
                                   const std::filesystem::path &right) noexcept
{
#ifdef _WIN32
    const auto &left_text = left.native();
    const auto &right_text = right.native();
    if (left_text.size() != right_text.size() || left_text.size() > INT_MAX)
    {
        return false;
    }
    return CompareStringOrdinal(left_text.data(), static_cast<int>(left_text.size()),
                                right_text.data(), static_cast<int>(right_text.size()),
                                TRUE) == CSTR_EQUAL;
#else
    return left.native() == right.native();
#endif
}

/// @brief Reports whether the first component sequence contains the second as a prefix.
[[nodiscard]] bool is_prefix(const std::vector<std::filesystem::path> &prefix,
                             const std::vector<std::filesystem::path> &path) noexcept
{
    return prefix.size() <= path.size() &&
           std::equal(prefix.begin(), prefix.end(), path.begin(), path_text_equal);
}

/// @brief Reports whether two claims identify overlapping resource hierarchies.
[[nodiscard]] bool overlaps(const ResourceClaim &left, const ResourceClaim &right) noexcept
{
    return path_text_equal(left.root, right.root) &&
           (is_prefix(left.components, right.components) ||
            is_prefix(right.components, left.components));
}

/// @brief Reports whether two overlapping modes may coexist.
[[nodiscard]] bool conflicts(const ResourceClaim &left, const ResourceClaim &right) noexcept
{
    return overlaps(left, right) &&
           (left.mode == FilesystemResourceAccessMode::exclusive_mutation ||
            right.mode == FilesystemResourceAccessMode::exclusive_mutation);
}

/// @brief Checks one active operation against a prepared claim set.
[[nodiscard]] bool claim_set_conflicts(const ActiveClaimSet &active,
                                       const std::vector<ResourceClaim> &requested_claims) noexcept
{
    return std::ranges::any_of(active.claims,
                               [&requested_claims](const ResourceClaim &existing)
                               {
                                   return std::ranges::any_of(
                                       requested_claims, [&existing](const ResourceClaim &requested)
                                       { return conflicts(existing, requested); });
                               });
}

/// @brief Checks a prepared claim set against every currently active operation.
[[nodiscard]] bool
conflicts_with_active(const std::vector<ActiveClaimSet> &active_claim_sets,
                      const std::vector<ResourceClaim> &requested_claims) noexcept
{
    return std::ranges::any_of(active_claim_sets, [&requested_claims](const ActiveClaimSet &active)
                               { return claim_set_conflicts(active, requested_claims); });
}

/// @brief Builds the canonical configured-root plus normalized-relative resource identity.
[[nodiscard]] Result<ResourceClaim, FilesystemResourceAccessError>
make_claim(const FilesystemResourceAccessRequest &request)
{
    if (request.root == nullptr)
    {
        return unexpected(FilesystemResourceAccessError{
            FilesystemResourceAccessErrorCode::invalid_path,
            filesystem::SafePathError{filesystem::SafePathErrorCode::resolution_failed, {}}});
    }
    auto safe_path = filesystem::SafePath::resolve(*request.root, request.requested_path);
    if (!safe_path)
    {
        return unexpected(FilesystemResourceAccessError{
            FilesystemResourceAccessErrorCode::invalid_path, std::move(safe_path).error()});
    }

    ResourceClaim claim{request.root->path(), {}, request.mode};
    const auto relative = safe_path->path().lexically_relative(request.root->path());
    claim.components.reserve(
        static_cast<std::size_t>(std::distance(relative.begin(), relative.end())));
    for (const auto &component : relative)
    {
        if (!component.empty() && component != ".")
        {
            claim.components.push_back(component);
        }
    }
    return claim;
}

/// @brief Provides a deterministic total order before atomic multi-resource acquisition.
[[nodiscard]] bool claim_less(const ResourceClaim &left, const ResourceClaim &right)
{
    const auto left_root = left.root.generic_u8string();
    const auto right_root = right.root.generic_u8string();
    if (left_root != right_root)
    {
        return left_root < right_root;
    }
    return std::lexicographical_compare(
        left.components.begin(), left.components.end(), right.components.begin(),
        right.components.end(), [](const auto &left_component, const auto &right_component)
        { return left_component.generic_u8string() < right_component.generic_u8string(); });
}

} // namespace

namespace detail
{

/// @brief Owns the bounded active-claim registry shared by coordinators and leases.
struct FilesystemResourceCoordinatorState
{
    std::mutex mutex;                   ///< Serializes registry inspection and mutation.
    std::vector<ActiveClaimSet> active; ///< Claims grouped by their owning operation.
    std::size_t active_claim_count{};   ///< Bounded total across active claim sets.
    std::uint64_t next_identifier{1};   ///< Next nonzero RAII release identifier.
};

} // namespace detail

FilesystemResourceLease::FilesystemResourceLease(
    std::shared_ptr<detail::FilesystemResourceCoordinatorState> state,
    const std::uint64_t identifier) noexcept
    : state_(std::move(state)), identifier_(identifier)
{
}

FilesystemResourceLease::FilesystemResourceLease(FilesystemResourceLease &&other) noexcept
    : state_(std::move(other.state_)), identifier_(std::exchange(other.identifier_, 0))
{
}

FilesystemResourceLease &
FilesystemResourceLease::operator=(FilesystemResourceLease &&other) noexcept
{
    if (this != &other)
    {
        release();
        state_ = std::move(other.state_);
        identifier_ = std::exchange(other.identifier_, 0);
    }
    return *this;
}

FilesystemResourceLease::~FilesystemResourceLease()
{
    release();
}

void FilesystemResourceLease::release() noexcept
{
    if (state_ == nullptr || identifier_ == 0)
    {
        return;
    }
    auto state = std::move(state_);
    const auto identifier = std::exchange(identifier_, 0);
    const std::lock_guard lock(state->mutex);
    const auto active = std::ranges::find(state->active, identifier, &ActiveClaimSet::identifier);
    if (active != state->active.end())
    {
        state->active_claim_count -= active->claims.size();
        state->active.erase(active);
    }
}

FilesystemResourceCoordinator::FilesystemResourceCoordinator()
    : state_(std::make_shared<detail::FilesystemResourceCoordinatorState>())
{
}

Result<FilesystemResourceLease, FilesystemResourceAccessError>
FilesystemResourceCoordinator::acquire(const filesystem::SharedRoot &root,
                                       const std::string_view requested_path,
                                       const FilesystemResourceAccessMode mode,
                                       const FilesystemResourceAccessOptions &options) const
{
    const FilesystemResourceAccessRequest request{&root, requested_path, mode};
    return acquire_all(std::span(&request, 1), options);
}

Result<FilesystemResourceLease, FilesystemResourceAccessError>
FilesystemResourceCoordinator::acquire_all(
    const std::span<const FilesystemResourceAccessRequest> requests,
    const FilesystemResourceAccessOptions &options) const
{
    if (const auto interrupted = interruption(options))
    {
        return unexpected(*interrupted);
    }
    if (requests.empty())
    {
        return unexpected(
            FilesystemResourceAccessError{FilesystemResourceAccessErrorCode::invalid_path, {}});
    }
    if (requests.size() > maximum_active_claims)
    {
        return unexpected(FilesystemResourceAccessError{
            FilesystemResourceAccessErrorCode::capacity_exceeded, {}});
    }
    {
        const std::lock_guard lock(state_->mutex);
        if (requests.size() > maximum_active_claims - state_->active_claim_count)
        {
            return unexpected(FilesystemResourceAccessError{
                FilesystemResourceAccessErrorCode::capacity_exceeded, {}});
        }
    }
    try
    {
        std::vector<ResourceClaim> claims;
        claims.reserve(requests.size());
        for (const auto &request : requests)
        {
            auto claim = make_claim(request);
            if (!claim)
            {
                const auto code = claim.error().code;
                auto path_error = std::move(claim).error().path_error;
                return unexpected(FilesystemResourceAccessError{code, std::move(path_error)});
            }
            claims.push_back(std::move(claim).value());
        }
        std::ranges::sort(claims, claim_less);

        if (const auto interrupted = interruption(options))
        {
            return unexpected(*interrupted);
        }
        const std::lock_guard lock(state_->mutex);
        // Cancellation may arrive while this operation is waiting briefly for registry access.
        // Recheck under the registry lock so ownership is never granted after interruption.
        if (const auto interrupted = interruption(options))
        {
            return unexpected(*interrupted);
        }
        if (claims.size() > maximum_active_claims - state_->active_claim_count)
        {
            return unexpected(FilesystemResourceAccessError{
                FilesystemResourceAccessErrorCode::capacity_exceeded, {}});
        }
        if (conflicts_with_active(state_->active, claims))
        {
            return unexpected(
                FilesystemResourceAccessError{FilesystemResourceAccessErrorCode::conflict, {}});
        }

        const auto identifier = state_->next_identifier++;
        state_->active.push_back({identifier, std::move(claims)});
        state_->active_claim_count += state_->active.back().claims.size();
        return FilesystemResourceLease(state_, identifier);
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(FilesystemResourceAccessError{
            FilesystemResourceAccessErrorCode::resource_allocation_failed, {}});
    }
}

} // namespace sparenode::application
