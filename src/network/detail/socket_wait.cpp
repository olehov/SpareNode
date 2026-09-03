#include "sparenode/network/detail/socket_wait.hpp"

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include "sparenode/network/detail/native_socket_owner.hpp"

namespace sparenode::network::detail
{

/// @brief Owns the connected datagram sockets used to signal cancellation.
struct SocketWakeChannel::Impl
{
    /// @brief Adopts the connected reader and writer sockets.
    /// @param[in] reader Reader socket whose ownership is transferred.
    /// @param[in] writer Writer socket whose ownership is transferred.
    Impl(NativeSocketOwner reader, NativeSocketOwner writer) noexcept
        : reader(std::move(reader)), writer(std::move(writer))
    {
    }

    /// @brief Socket drained by the waiting thread after a stop request.
    NativeSocketOwner reader;
    /// @brief Socket used by the stop callback to send a wake byte.
    NativeSocketOwner writer;
};

SocketWakeChannel::SocketWakeChannel() noexcept = default;

SocketWakeChannel::~SocketWakeChannel() = default;

Result<std::unique_ptr<SocketWakeChannel::Impl>, NetworkError>
SocketWakeChannel::create(const NetworkOperation operation)
{
    NativeSocketOwner reader_socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (reader_socket.get() == invalid_socket)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }
    if (!configure_socket_nonblocking(reader_socket.get()))
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    sockaddr_in reader_endpoint{};
    reader_endpoint.sin_family = AF_INET;
    reader_endpoint.sin_port = 0;
    reader_endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(reader_socket.get(), reinterpret_cast<const sockaddr *>(&reader_endpoint),
               static_cast<SocketLength>(sizeof(reader_endpoint))) != 0)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    auto reader_endpoint_length = static_cast<SocketLength>(sizeof(reader_endpoint));
    if (::getsockname(reader_socket.get(), reinterpret_cast<sockaddr *>(&reader_endpoint),
                      &reader_endpoint_length) != 0)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    NativeSocketOwner writer_socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (writer_socket.get() == invalid_socket)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }
    if (!configure_socket_nonblocking(writer_socket.get()))
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    sockaddr_in writer_endpoint{};
    writer_endpoint.sin_family = AF_INET;
    writer_endpoint.sin_port = 0;
    writer_endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(writer_socket.get(), reinterpret_cast<const sockaddr *>(&writer_endpoint),
               static_cast<SocketLength>(sizeof(writer_endpoint))) != 0)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    auto writer_endpoint_length = static_cast<SocketLength>(sizeof(writer_endpoint));
    if (::getsockname(writer_socket.get(), reinterpret_cast<sockaddr *>(&writer_endpoint),
                      &writer_endpoint_length) != 0)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    // Connecting both endpoints makes the kernel discard datagrams from any
    // unrelated local process before they can make the wake reader readable.
    if (::connect(reader_socket.get(), reinterpret_cast<const sockaddr *>(&writer_endpoint),
                  static_cast<SocketLength>(sizeof(writer_endpoint))) != 0 ||
        ::connect(writer_socket.get(), reinterpret_cast<const sockaddr *>(&reader_endpoint),
                  static_cast<SocketLength>(sizeof(reader_endpoint))) != 0)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    return std::make_unique<Impl>(std::move(reader_socket), std::move(writer_socket));
}

Result<void, NetworkError> SocketWakeChannel::ensure_initialized(const NetworkOperation operation)
{
    if (impl_ != nullptr)
    {
        return {};
    }

    auto channel_result = create(operation);
    if (!channel_result)
    {
        return unexpected(channel_result.error());
    }

    impl_ = std::move(channel_result.value());
    return {};
}

void SocketWakeChannel::notify() const noexcept
{
    if (impl_ == nullptr)
    {
        return;
    }

    constexpr char wake_byte = 1;
    static_cast<void>(::send(impl_->writer.get(), &wake_byte, 1, 0));
}

void SocketWakeChannel::drain() const noexcept
{
    if (impl_ == nullptr)
    {
        return;
    }

    std::array<char, 16> wake_bytes{};
    while (::recv(impl_->reader.get(), wake_bytes.data(), static_cast<int>(wake_bytes.size()), 0) >
           0)
    {
    }
}

NativeSocket SocketWakeChannel::reader() const noexcept
{
    return impl_ == nullptr ? invalid_socket : impl_->reader.get();
}

bool SocketWakeChannel::is_initialized() const noexcept
{
    return impl_ != nullptr;
}

namespace
{

/// @brief Constructs the portable readiness request for one operation socket.
/// @param[in] socket Borrowed operation socket.
/// @param[in] interest Read or write readiness requested by the operation.
/// @return Initialized portable poll entry.
[[nodiscard]] SocketPollEntry operation_entry(const NativeSocket socket,
                                              const SocketWaitInterest interest) noexcept
{
    SocketPollEntry entry{.socket = socket};
    entry.watch_readable = interest == SocketWaitInterest::readable;
    entry.watch_writable = interest == SocketWaitInterest::writable;
    return entry;
}

/// @brief Converts poll output to a status the native operation can interpret.
/// @param[in] entry Completed portable poll entry.
/// @param[in] interest Read or write readiness requested by the operation.
/// @return A recognized completion status, or no value for unexplained flags.
[[nodiscard]] std::optional<SocketWaitStatus>
ready_status(const SocketPollEntry &entry, const SocketWaitInterest interest) noexcept
{
    if (entry.error)
    {
        return SocketWaitStatus::socket_error;
    }
    if (entry.hangup)
    {
        return SocketWaitStatus::socket_hangup;
    }

    const bool requested_event =
        interest == SocketWaitInterest::readable ? entry.readable : entry.writable;
    if (requested_event)
    {
        return SocketWaitStatus::socket_ready;
    }

    return std::nullopt;
}

/// @brief Produces an error for an invalid or unexplained poll result.
/// @param[in] operation Public operation associated with the failed wait.
/// @return Structured socket-domain error.
[[nodiscard]] NetworkError invalid_wait_result(const NetworkOperation operation) noexcept
{
    return NetworkError{operation, NetworkErrorDomain::socket, 0};
}

/// @brief Describes how a cancellable wait should proceed after inspecting its wake entry.
enum class WakeEntryStatus : std::uint8_t
{
    inspect_operation,
    retry,
    cancelled,
};

/// @brief Drains and validates the cancellation wake entry after a completed poll.
/// @param[in] context Stable socket wait resources, including the wake channel.
/// @param[in] entry Completed wake-channel poll entry.
/// @param[in] operation Public operation associated with the wait.
/// @param[in] stop_token Token checked after draining a wake notification.
/// @return The next wait action, or a structured wake-channel error.
[[nodiscard]] Result<WakeEntryStatus, NetworkError>
inspect_wake_entry(const SocketWaitContext &context, const SocketPollEntry &entry,
                   const NetworkOperation operation, const std::stop_token &stop_token)
{
    if (entry.readable)
    {
        context.wake_channel.drain();
    }

    if (stop_token.stop_requested())
    {
        return WakeEntryStatus::cancelled;
    }

    if (entry.invalid || entry.error || entry.hangup)
    {
        return unexpected(invalid_wait_result(operation));
    }

    return entry.readable ? WakeEntryStatus::retry : WakeEntryStatus::inspect_operation;
}

/// @brief Interprets one completed operation-socket descriptor.
/// @param[in] entry Completed operation entry.
/// @param[in] request Requested interest and public operation metadata.
/// @return Readiness status or a structured invalid-result error.
[[nodiscard]] Result<SocketWaitStatus, NetworkError>
inspect_operation_entry(const SocketPollEntry &entry, const SocketWaitRequest request)
{
    if (entry.invalid)
    {
        return unexpected(invalid_wait_result(request.operation));
    }
    if (const auto status = ready_status(entry, request.interest); status.has_value())
    {
        return status.value();
    }
    return unexpected(invalid_wait_result(request.operation));
}

/// @brief Waits for one operation socket without allocating a cancellation channel.
/// @param[in] context Stable socket wait resources.
/// @param[in] request Requested interest and public operation metadata.
/// @param[in] deadline Optional absolute monotonic expiry.
/// @return Readiness or timeout status, or a structured poll error.
[[nodiscard]] Result<SocketWaitStatus, NetworkError>
wait_for_operation(const SocketWaitContext &context, const SocketWaitRequest request,
                   const std::optional<NetworkDeadline> deadline)
{
    std::array<SocketPollEntry, 1> descriptors{{
        operation_entry(context.socket, request.interest),
    }};
    const auto poll_result = context.poller.wait(descriptors, request.operation, deadline);
    if (!poll_result)
    {
        return unexpected(poll_result.error());
    }
    if (poll_result.value() == SocketPollStatus::timed_out)
    {
        return SocketWaitStatus::timed_out;
    }
    return inspect_operation_entry(descriptors[0], request);
}

} // namespace

Result<SocketWaitStatus, NetworkError> wait_for_socket(const SocketWaitContext &context,
                                                       const SocketWaitRequest request)
{
    return wait_for_operation(context, request, std::nullopt);
}

Result<SocketWaitStatus, NetworkError> wait_for_socket(const SocketWaitContext &context,
                                                       const SocketWaitRequest request,
                                                       const std::stop_token &stop_token)
{
    return wait_for_socket(context, request,
                           NetworkIoOptions{.stop_token = stop_token, .deadline = std::nullopt});
}

Result<SocketWaitStatus, NetworkError> wait_for_socket(const SocketWaitContext &context,
                                                       const SocketWaitRequest request,
                                                       const NetworkIoOptions &options)
{
    if (options.stop_token.stop_requested())
    {
        return SocketWaitStatus::cancelled;
    }
    if (options.deadline.has_value() &&
        std::chrono::steady_clock::now() >= options.deadline.value())
    {
        return SocketWaitStatus::timed_out;
    }

    if (!options.stop_token.stop_possible())
    {
        return wait_for_operation(context, request, options.deadline);
    }

    if (auto initialized = context.wake_channel.ensure_initialized(request.operation); !initialized)
    {
        return unexpected(initialized.error());
    }

    const std::stop_callback wake_on_stop(options.stop_token,
                                          [&context] { context.wake_channel.notify(); });
    std::array<SocketPollEntry, 2> descriptors{{
        operation_entry(context.socket, request.interest),
        {.socket = context.wake_channel.reader(), .watch_readable = true},
    }};

    while (true)
    {
        const auto poll_result =
            context.poller.wait(descriptors, request.operation, options.deadline);
        if (!poll_result)
        {
            return unexpected(poll_result.error());
        }
        if (poll_result.value() == SocketPollStatus::timed_out)
        {
            return options.stop_token.stop_requested() ? SocketWaitStatus::cancelled
                                                       : SocketWaitStatus::timed_out;
        }

        const auto wake_status =
            inspect_wake_entry(context, descriptors[1], request.operation, options.stop_token);
        if (!wake_status)
        {
            return unexpected(wake_status.error());
        }

        switch (wake_status.value())
        {
        case WakeEntryStatus::cancelled:
            return SocketWaitStatus::cancelled;
        case WakeEntryStatus::retry:
            continue;
        case WakeEntryStatus::inspect_operation:
            break;
        }

        return inspect_operation_entry(descriptors[0], request);
    }
}

} // namespace sparenode::network::detail
