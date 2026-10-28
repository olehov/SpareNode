#pragma once

#include <cstddef>
#include <optional>

#include "sparenode/filesystem/upload_finalizer.hpp"
#include "sparenode/foundation/concurrency/interruption.hpp"

namespace sparenode::filesystem::detail
{

/// @brief Bounds memory used by every platform upload-copy implementation.
inline constexpr std::size_t upload_copy_buffer_bytes = std::size_t{64} * 1024;

/// @brief Bounds collisions retried while creating an exclusive staging sibling.
inline constexpr std::size_t maximum_upload_stage_attempts = 100;

/// @brief Maps dependency-neutral interruption state into upload finalization errors.
[[nodiscard]] inline std::optional<UploadFinalizationError>
upload_interruption(const storage::TemporaryFileIoOptions &options) noexcept
{
    switch (concurrency::interruption_status(options.stop_token, options.deadline))
    {
    case concurrency::InterruptionStatus::cancelled:
        return UploadFinalizationError{UploadFinalizationErrorCode::cancelled};
    case concurrency::InterruptionStatus::deadline_exceeded:
        return UploadFinalizationError{UploadFinalizationErrorCode::deadline_exceeded};
    case concurrency::InterruptionStatus::none:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace sparenode::filesystem::detail
