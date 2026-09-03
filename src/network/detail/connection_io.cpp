#include "sparenode/network/detail/connection_io.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>

namespace sparenode::network::detail
{
namespace
{

/// @brief Reports readiness states that must not be retried after would-block.
/// @param[in] status Status returned by the preceding readiness wait.
/// @return `true` when another readiness wait cannot make progress.
[[nodiscard]] bool terminal_readiness(const SocketWaitStatus status) noexcept
{
    return status == SocketWaitStatus::socket_error || status == SocketWaitStatus::socket_hangup;
}

/// @brief Implements readiness, cancellation, and retry policy for one transfer.
/// @tparam Transfer Nullary callable that performs one native transfer attempt.
/// @param[in] context Borrowed socket and transfer collaborators.
/// @param[in] request Readiness interest and public operation metadata.
/// @param[in] stop_token Token observed while waiting for readiness.
/// @param[in] transfer Transfer callable invoked after any non-cancelled wait,
/// including socket-error and hangup statuses, so the native call reports the outcome.
/// @return Transferred byte count, or a structured network error.
template <typename Transfer>
[[nodiscard]] Result<std::size_t, NetworkError>
perform_io(const ConnectionIoContext &context, const SocketWaitRequest request,
           const NetworkIoOptions &options, Transfer transfer)
{
    while (true)
    {
        const auto wait_result = wait_for_socket_with_options(context.wait, request, options);
        if (!wait_result)
        {
            return unexpected(wait_result.error());
        }
        if (wait_result.value() == SocketWaitStatus::cancelled)
        {
            return unexpected(NetworkError{request.operation, NetworkErrorDomain::cancellation, 0});
        }
        if (wait_result.value() == SocketWaitStatus::timed_out)
        {
            return unexpected(NetworkError{request.operation, NetworkErrorDomain::timeout, 0});
        }

        const std::ptrdiff_t transferred = transfer();
        if (transferred >= 0)
        {
            return static_cast<std::size_t>(transferred);
        }

        const int error_code = context.operations.last_error();
        if (socket_error_interrupted(error_code))
        {
            continue;
        }
        if (socket_error_would_block(error_code) && !terminal_readiness(wait_result.value()))
        {
            continue;
        }

        return unexpected(NetworkError{request.operation, NetworkErrorDomain::socket, error_code});
    }
}

} // namespace

std::ptrdiff_t NativeSocketOperations::receive(const NativeSocket socket,
                                               const std::span<std::byte> buffer) noexcept
{
    return receive_socket(socket, buffer);
}

std::ptrdiff_t NativeSocketOperations::send(const NativeSocket socket,
                                            const std::span<const std::byte> buffer) noexcept
{
    return send_socket(socket, buffer);
}

int NativeSocketOperations::last_error() const noexcept
{
    return last_socket_error();
}

ConnectionIo::ConnectionIo(const ConnectionIoContext &context) noexcept : context_(context)
{
}

Result<std::size_t, NetworkError> ConnectionIo::receive(const std::span<std::byte> buffer,
                                                        const std::stop_token &stop_token)
{
    return receive_with_options(
        buffer, NetworkIoOptions{.stop_token = stop_token, .deadline = std::nullopt});
}

Result<std::size_t, NetworkError>
ConnectionIo::receive_with_options(const std::span<std::byte> buffer,
                                   const NetworkIoOptions &options)
{
    return perform_io(
        context_,
        {.interest = SocketWaitInterest::readable, .operation = NetworkOperation::receive}, options,
        [this, buffer] { return context_.operations.receive(context_.wait.socket, buffer); });
}

Result<std::size_t, NetworkError> ConnectionIo::send(const std::span<const std::byte> buffer,
                                                     const std::stop_token &stop_token)
{
    return send_with_options(buffer,
                             NetworkIoOptions{.stop_token = stop_token, .deadline = std::nullopt});
}

Result<std::size_t, NetworkError>
ConnectionIo::send_with_options(const std::span<const std::byte> buffer,
                                const NetworkIoOptions &options)
{
    return perform_io(
        context_, {.interest = SocketWaitInterest::writable, .operation = NetworkOperation::send},
        options, [this, buffer] { return context_.operations.send(context_.wait.socket, buffer); });
}

} // namespace sparenode::network::detail
