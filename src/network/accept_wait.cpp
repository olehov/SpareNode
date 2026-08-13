#include "sparenode/network/detail/accept_wait.hpp"

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

    [[nodiscard]] NativeSocket reader() const noexcept
    {
        return reader_.get();
    }

  private:
    NativeSocketOwner reader_;
    NativeSocketOwner writer_;
};

/// Creates two UDP sockets connected over IPv4 loopback for portable wake-ups.
[[nodiscard]] Result<WakeChannel, NetworkError> create_wake_channel()
{
    NativeSocketOwner reader(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (reader.get() == invalid_socket)
    {
        return unexpected(NetworkError{
            NetworkOperation::accept,
            NetworkErrorDomain::socket,
            last_socket_error(),
        });
    }

    sockaddr_in reader_endpoint{};
    reader_endpoint.sin_family = AF_INET;
    reader_endpoint.sin_port = 0;
    reader_endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(reader.get(), reinterpret_cast<const sockaddr *>(&reader_endpoint),
               static_cast<SocketLength>(sizeof(reader_endpoint))) != 0)
    {
        return unexpected(NetworkError{
            NetworkOperation::accept,
            NetworkErrorDomain::socket,
            last_socket_error(),
        });
    }

    auto reader_endpoint_length = static_cast<SocketLength>(sizeof(reader_endpoint));
    if (::getsockname(reader.get(), reinterpret_cast<sockaddr *>(&reader_endpoint),
                      &reader_endpoint_length) != 0)
    {
        return unexpected(NetworkError{
            NetworkOperation::accept,
            NetworkErrorDomain::socket,
            last_socket_error(),
        });
    }

    NativeSocketOwner writer(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (writer.get() == invalid_socket)
    {
        return unexpected(NetworkError{
            NetworkOperation::accept,
            NetworkErrorDomain::socket,
            last_socket_error(),
        });
    }

    sockaddr_in writer_endpoint{};
    writer_endpoint.sin_family = AF_INET;
    writer_endpoint.sin_port = 0;
    writer_endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(writer.get(), reinterpret_cast<const sockaddr *>(&writer_endpoint),
               static_cast<SocketLength>(sizeof(writer_endpoint))) != 0)
    {
        return unexpected(NetworkError{
            NetworkOperation::accept,
            NetworkErrorDomain::socket,
            last_socket_error(),
        });
    }

    auto writer_endpoint_length = static_cast<SocketLength>(sizeof(writer_endpoint));
    if (::getsockname(writer.get(), reinterpret_cast<sockaddr *>(&writer_endpoint),
                      &writer_endpoint_length) != 0)
    {
        return unexpected(NetworkError{
            NetworkOperation::accept,
            NetworkErrorDomain::socket,
            last_socket_error(),
        });
    }

    // Connecting both endpoints makes the kernel discard datagrams from any
    // unrelated local process before they can make the wake reader readable.
    if (::connect(reader.get(), reinterpret_cast<const sockaddr *>(&writer_endpoint),
                  static_cast<SocketLength>(sizeof(writer_endpoint))) != 0)
    {
        return unexpected(NetworkError{
            NetworkOperation::accept,
            NetworkErrorDomain::socket,
            last_socket_error(),
        });
    }

    if (::connect(writer.get(), reinterpret_cast<const sockaddr *>(&reader_endpoint),
                  static_cast<SocketLength>(sizeof(reader_endpoint))) != 0)
    {
        return unexpected(NetworkError{
            NetworkOperation::accept,
            NetworkErrorDomain::socket,
            last_socket_error(),
        });
    }

    return WakeChannel(std::move(reader), std::move(writer));
}

} // namespace

Result<AcceptWaitStatus, NetworkError> wait_for_accept(const NativeSocket listener_socket,
                                                       SocketPoller &poller)
{
    std::array<SocketPollEntry, 1> descriptors{{
        {.socket = listener_socket, .watch_readable = true},
    }};

    const auto poll_result = poller.wait(descriptors, NetworkOperation::accept);
    if (!poll_result)
    {
        return unexpected(poll_result.error());
    }

    if (descriptors[0].readable)
    {
        return AcceptWaitStatus::socket_ready;
    }

    return unexpected(NetworkError{NetworkOperation::accept, NetworkErrorDomain::socket, 0});
}

Result<AcceptWaitStatus, NetworkError> wait_for_accept(const NativeSocket listener_socket,
                                                       const std::stop_token &stop_token,
                                                       SocketPoller &poller)
{
    if (stop_token.stop_requested())
    {
        return AcceptWaitStatus::cancelled;
    }

    auto channel_result = create_wake_channel();
    if (!channel_result)
    {
        return unexpected(channel_result.error());
    }

    auto &channel = channel_result.value();
    const std::stop_callback wake_on_stop(stop_token, [&channel] { channel.notify(); });

    std::array<SocketPollEntry, 2> descriptors{{
        {.socket = listener_socket, .watch_readable = true},
        {.socket = channel.reader(), .watch_readable = true},
    }};

    while (true)
    {
        const auto poll_result = poller.wait(descriptors, NetworkOperation::accept);
        if (!poll_result)
        {
            return unexpected(poll_result.error());
        }

        if (descriptors[1].readable)
        {
            // Always drain the authenticated wake before leaving the wait. This
            // also lets an in-flight stop callback finish before its channel closes.
            char wake_byte{};
            static_cast<void>(::recv(channel.reader(), &wake_byte, 1, 0));
        }

        // Cancellation wins when it races with an incoming connection.
        if (stop_token.stop_requested())
        {
            return AcceptWaitStatus::cancelled;
        }

        if (descriptors[1].readable)
        {
            // Socket readability without a stop request is not cancellation.
            continue;
        }

        if (descriptors[0].readable)
        {
            return AcceptWaitStatus::socket_ready;
        }

        return unexpected(NetworkError{
            NetworkOperation::accept,
            NetworkErrorDomain::socket,
            0,
        });
    }
}

} // namespace sparenode::network::detail
