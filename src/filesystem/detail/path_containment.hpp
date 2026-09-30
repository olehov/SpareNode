#pragma once

#include <filesystem>
#include <functional>

namespace sparenode::filesystem::detail
{

/// @brief Gives a root path a distinct type at containment call sites.
struct PathBoundary
{
    const std::filesystem::path &path; ///< Root required as a complete component prefix.
};

/// @brief Checks path containment using complete native path components.
/// @tparam ComponentEqual Callable implementing the host's component equality rules.
/// @param[in] candidate Absolute normalized path whose prefix is inspected.
/// @param[in] root Absolute normalized root required as the complete prefix.
/// @param[in] component_equal Component comparison policy.
/// @return `true` when candidate is the root or one of its descendants.
template <typename ComponentEqual = std::equal_to<>>
[[nodiscard]] bool is_within_root(const std::filesystem::path &candidate, const PathBoundary root,
                                  ComponentEqual component_equal = {}) noexcept
{
    auto candidate_component = candidate.begin();
    for (const auto &root_component : root.path)
    {
        if (candidate_component == candidate.end() ||
            !component_equal(*candidate_component, root_component))
        {
            return false;
        }
        ++candidate_component;
    }
    return true;
}

} // namespace sparenode::filesystem::detail
