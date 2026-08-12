#include "sparenode/network/detail/accept_wait.hpp"

#include <array>
#include <stop_token>
#include <utility>

#include "sparenode/network/detail/native_socket_owner.hpp"

#ifndef _WIN32
#include <cerrno>
#include <poll.h>
#endif

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

    sockaddr_in loopback{};
    loopback.sin_family = AF_INET;
    loopback.sin_port = 0;
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(reader.get(), reinterpret_cast<const sockaddr *>(&loopback),
               static_cast<SocketLength>(sizeof(loopback))) != 0)
    {
        return unexpected(NetworkError{
            NetworkOperation::accept,
            NetworkErrorDomain::socket,
            last_socket_error(),
        });
    }

    auto loopback_length = static_cast<SocketLength>(sizeof(loopback));
    if (::getsockname(reader.get(), reinterpret_cast<sockaddr *>(&loopback), &loopback_length) != 0)
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

    if (::connect(writer.get(), reinterpret_cast<const sockaddr *>(&loopback),
                  static_cast<SocketLength>(sizeof(loopback))) != 0)
    {
        return unexpected(NetworkError{
            NetworkOperation::accept,
            NetworkErrorDomain::socket,
            last_socket_error(),
        });
    }

    return WakeChannel(std::move(reader), std::move(writer));
}

#ifdef _WIN32
using PollDescriptor = WSAPOLLFD;
inline constexpr short readable_event = POLLRDNORM;

[[nodiscard]] int poll_descriptors(PollDescriptor *descriptors) noexcept
{
    return WSAPoll(descriptors, 2, -1);
}

[[nodiscard]] bool poll_was_interrupted(const int error_code) noexcept
{
    return error_code == WSAEINTR;
}
#else
using PollDescriptor = pollfd;
inline constexpr short readable_event = POLLIN;

[[nodiscard]] int poll_descriptors(PollDescriptor *descriptors) noexcept
{
    return ::poll(descriptors, 2, -1);
}

[[nodiscard]] bool poll_was_interrupted(const int error_code) noexcept
{
    return error_code == EINTR;
}
#endif

} // namespace

Result<AcceptWaitStatus, NetworkError> wait_for_accept(const NativeSocket listener_socket,
                                                       const std::stop_token &stop_token)
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

    std::array<PollDescriptor, 2> descriptors{{
        {listener_socket, readable_event, 0},
        {channel.reader(), readable_event, 0},
    }};

    while (true)
    {
        const int poll_result = poll_descriptors(descriptors.data());
        if (poll_result < 0)
        {
            const int error_code = last_socket_error();
            if (poll_was_interrupted(error_code))
            {
                continue;
            }

            return unexpected(NetworkError{
                NetworkOperation::accept,
                NetworkErrorDomain::socket,
                error_code,
            });
        }

        // Cancellation wins when it races with an incoming connection.
        if (stop_token.stop_requested() || (descriptors[1].revents & readable_event) != 0)
        {
            return AcceptWaitStatus::cancelled;
        }

        if ((descriptors[0].revents & readable_event) != 0)
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
