#include "sparenode/network/detail/connection_io.hpp"

#include <cstddef>
#include <span>
#include <stop_token>
#include <utility>

namespace sparenode::network::detail
{
namespace
{

/// Reports readiness states that must not be retried after would-block.
[[nodiscard]] bool terminal_readiness(const SocketWaitStatus status) noexcept
{
    return status == SocketWaitStatus::socket_error || status == SocketWaitStatus::socket_hangup;
}

/// Implements the shared readiness, cancellation, and retry policy for one transfer.
template <typename Transfer>
[[nodiscard]] Result<std::size_t, NetworkError>
perform_io(const ConnectionIoContext context, const SocketWaitRequest request,
           const std::stop_token &stop_token, Transfer transfer)
{
    while (true)
    {
        const auto wait_result = wait_for_socket(context.wait, request, stop_token);
        if (!wait_result)
        {
            return unexpected(wait_result.error());
        }
        if (wait_result.value() == SocketWaitStatus::cancelled)
        {
            return unexpected(NetworkError{request.operation, NetworkErrorDomain::cancellation, 0});
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

ConnectionIo::ConnectionIo(const ConnectionIoContext context) noexcept : context_(context)
{
}

Result<std::size_t, NetworkError> ConnectionIo::receive(const std::span<std::byte> buffer,
                                                        const std::stop_token &stop_token)
{
    return perform_io(
        context_,
        {.interest = SocketWaitInterest::readable, .operation = NetworkOperation::receive},
        stop_token,
        [this, buffer] { return context_.operations.receive(context_.wait.socket, buffer); });
}

Result<std::size_t, NetworkError> ConnectionIo::send(const std::span<const std::byte> buffer,
                                                     const std::stop_token &stop_token)
{
    return perform_io(
        context_, {.interest = SocketWaitInterest::writable, .operation = NetworkOperation::send},
        stop_token,
        [this, buffer] { return context_.operations.send(context_.wait.socket, buffer); });
}

} // namespace sparenode::network::detail
