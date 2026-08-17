#include "sparenode/network/detail/socket_wait.hpp"

#include <array>
#include <stop_token>
#include <utility>

#include "sparenode/network/detail/native_socket_owner.hpp"

namespace sparenode::network::detail
{
namespace
{

/// Owns a loopback-only datagram pair whose readable end participates in poll.
class WakeChannel final
{
  public:
    WakeChannel(NativeSocketOwner reader, NativeSocketOwner writer) noexcept
        : reader_(std::move(reader)), writer_(std::move(writer))
    {
    }

    WakeChannel(const WakeChannel &) = delete;
    WakeChannel &operator=(const WakeChannel &) = delete;
    WakeChannel(WakeChannel &&) noexcept = default;
    WakeChannel &operator=(WakeChannel &&) = delete;

    /// Sends one byte to make the reader socket immediately readable.
    void notify() const noexcept
    {
        constexpr char wake_byte = 1;
        static_cast<void>(::send(writer_.get(), &wake_byte, 1, 0));
    }

    /// Returns the descriptor watched alongside the operation socket.
    [[nodiscard]] NativeSocket reader() const noexcept
    {
        return reader_.get();
    }

  private:
    NativeSocketOwner reader_;
    NativeSocketOwner writer_;
};

/// Creates two authenticated UDP sockets connected over IPv4 loopback.
[[nodiscard]] Result<WakeChannel, NetworkError>
create_wake_channel(const NetworkOperation operation)
{
    NativeSocketOwner reader(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (reader.get() == invalid_socket)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    sockaddr_in reader_endpoint{};
    reader_endpoint.sin_family = AF_INET;
    reader_endpoint.sin_port = 0;
    reader_endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(reader.get(), reinterpret_cast<const sockaddr *>(&reader_endpoint),
               static_cast<SocketLength>(sizeof(reader_endpoint))) != 0)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    auto reader_endpoint_length = static_cast<SocketLength>(sizeof(reader_endpoint));
    if (::getsockname(reader.get(), reinterpret_cast<sockaddr *>(&reader_endpoint),
                      &reader_endpoint_length) != 0)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    NativeSocketOwner writer(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (writer.get() == invalid_socket)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    sockaddr_in writer_endpoint{};
    writer_endpoint.sin_family = AF_INET;
    writer_endpoint.sin_port = 0;
    writer_endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(writer.get(), reinterpret_cast<const sockaddr *>(&writer_endpoint),
               static_cast<SocketLength>(sizeof(writer_endpoint))) != 0)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    auto writer_endpoint_length = static_cast<SocketLength>(sizeof(writer_endpoint));
    if (::getsockname(writer.get(), reinterpret_cast<sockaddr *>(&writer_endpoint),
                      &writer_endpoint_length) != 0)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    // Connecting both endpoints makes the kernel discard datagrams from any
    // unrelated local process before they can make the wake reader readable.
    if (::connect(reader.get(), reinterpret_cast<const sockaddr *>(&writer_endpoint),
                  static_cast<SocketLength>(sizeof(writer_endpoint))) != 0 ||
        ::connect(writer.get(), reinterpret_cast<const sockaddr *>(&reader_endpoint),
                  static_cast<SocketLength>(sizeof(reader_endpoint))) != 0)
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::socket, last_socket_error()});
    }

    return WakeChannel(std::move(reader), std::move(writer));
}

/// Constructs the portable readiness request for one operation socket.
[[nodiscard]] SocketPollEntry operation_entry(const NativeSocket socket,
                                              const SocketWaitInterest interest) noexcept
{
    SocketPollEntry entry{.socket = socket};
    entry.watch_readable = interest == SocketWaitInterest::readable;
    entry.watch_writable = interest == SocketWaitInterest::writable;
    return entry;
}

/// Reports events that should be interpreted by the subsequent socket call.
[[nodiscard]] bool operation_may_proceed(const SocketPollEntry &entry,
                                         const SocketWaitInterest interest) noexcept
{
    const bool requested_event =
        interest == SocketWaitInterest::readable ? entry.readable : entry.writable;
    return requested_event || entry.error || entry.hangup;
}

/// Produces a structured error for an invalid or unexplained poll result.
[[nodiscard]] NetworkError invalid_wait_result(const NetworkOperation operation) noexcept
{
    return NetworkError{operation, NetworkErrorDomain::socket, 0};
}

} // namespace

Result<SocketWaitStatus, NetworkError> wait_for_socket(const NativeSocket socket,
                                                       const SocketWaitInterest interest,
                                                       const NetworkOperation operation,
                                                       SocketPoller &poller)
{
    std::array<SocketPollEntry, 1> descriptors{{operation_entry(socket, interest)}};
    const auto poll_result = poller.wait(descriptors, operation);
    if (!poll_result)
    {
        return unexpected(poll_result.error());
    }

    if (operation_may_proceed(descriptors[0], interest))
    {
        return SocketWaitStatus::socket_ready;
    }

    return unexpected(invalid_wait_result(operation));
}

Result<SocketWaitStatus, NetworkError> wait_for_socket(const NativeSocket socket,
                                                       const SocketWaitInterest interest,
                                                       const NetworkOperation operation,
                                                       const std::stop_token &stop_token,
                                                       SocketPoller &poller)
{
    if (stop_token.stop_requested())
    {
        return SocketWaitStatus::cancelled;
    }

    if (!stop_token.stop_possible())
    {
        return wait_for_socket(socket, interest, operation, poller);
    }

    auto channel_result = create_wake_channel(operation);
    if (!channel_result)
    {
        return unexpected(channel_result.error());
    }

    auto &channel = channel_result.value();
    const std::stop_callback wake_on_stop(stop_token, [&channel] { channel.notify(); });
    std::array<SocketPollEntry, 2> descriptors{{
        operation_entry(socket, interest),
        {.socket = channel.reader(), .watch_readable = true},
    }};

    while (true)
    {
        const auto poll_result = poller.wait(descriptors, operation);
        if (!poll_result)
        {
            return unexpected(poll_result.error());
        }

        if (descriptors[1].readable)
        {
            // Draining lets an in-flight callback finish before its channel closes.
            char wake_byte{};
            static_cast<void>(::recv(channel.reader(), &wake_byte, 1, 0));
        }

        if (stop_token.stop_requested())
        {
            return SocketWaitStatus::cancelled;
        }

        if (descriptors[1].readable)
        {
            continue;
        }

        if (operation_may_proceed(descriptors[0], interest))
        {
            return SocketWaitStatus::socket_ready;
        }

        return unexpected(invalid_wait_result(operation));
    }
}

} // namespace sparenode::network::detail
