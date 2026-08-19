#include "sparenode/configuration/shared_root.hpp"

#include <utility>

namespace sparenode::configuration
{

SharedRoot::SharedRoot(std::filesystem::path canonical_path) : path_(std::move(canonical_path))
{
}

Result<SharedRoot, SharedRootError> SharedRoot::create(const std::filesystem::path &input_path)
{
    if (input_path.empty())
    {
        return unexpected(SharedRootError{SharedRootErrorCode::empty_path, input_path});
    }

    std::error_code filesystem_error;
    const bool path_exists = std::filesystem::exists(input_path, filesystem_error);
    if (filesystem_error)
    {
        return unexpected(SharedRootError{SharedRootErrorCode::canonicalization_failed, input_path,
                                          filesystem_error});
    }
    if (!path_exists)
    {
        return unexpected(SharedRootError{SharedRootErrorCode::not_found, input_path});
    }

    const bool path_is_directory = std::filesystem::is_directory(input_path, filesystem_error);
    if (filesystem_error)
    {
        return unexpected(SharedRootError{SharedRootErrorCode::canonicalization_failed, input_path,
                                          filesystem_error});
    }
    if (!path_is_directory)
    {
        return unexpected(SharedRootError{SharedRootErrorCode::not_directory, input_path});
    }

    auto canonical_path = std::filesystem::canonical(input_path, filesystem_error);
    if (filesystem_error)
    {
        return unexpected(SharedRootError{SharedRootErrorCode::canonicalization_failed, input_path,
                                          filesystem_error});
    }

    return SharedRoot(std::move(canonical_path));
}

const std::filesystem::path &SharedRoot::path() const noexcept
{
    return path_;
}

const char *to_string(const SharedRootErrorCode code) noexcept
{
    switch (code)
    {
    case SharedRootErrorCode::empty_path:
        return "the shared-root path is empty";
    case SharedRootErrorCode::not_found:
        return "the shared-root path does not exist";
    case SharedRootErrorCode::not_directory:
        return "the shared-root path is not a directory";
    case SharedRootErrorCode::canonicalization_failed:
        return "the shared-root path could not be resolved";
    }

    return "unknown shared-root error";
}

} // namespace sparenode::configuration
