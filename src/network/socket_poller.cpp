#include "sparenode/network/detail/socket_poller.hpp"

#include <cerrno>
#include <vector>

#ifndef _WIN32
#include <poll.h>
#endif

namespace sparenode::network::detail
{
namespace
{

#ifdef _WIN32
using NativePollEntry = WSAPOLLFD;
inline constexpr short native_readable_event = POLLRDNORM;
inline constexpr short native_writable_event = POLLWRNORM;

[[nodiscard]] int poll_native_entries(std::vector<NativePollEntry> &entries) noexcept
{
    return WSAPoll(entries.data(), static_cast<ULONG>(entries.size()), -1);
}

[[nodiscard]] bool poll_was_interrupted(const int error_code) noexcept
{
    return error_code == WSAEINTR;
}
#else
using NativePollEntry = pollfd;
inline constexpr short native_readable_event = POLLIN;
inline constexpr short native_writable_event = POLLOUT;

[[nodiscard]] int poll_native_entries(std::vector<NativePollEntry> &entries) noexcept
{
    return ::poll(entries.data(), static_cast<nfds_t>(entries.size()), -1);
}

[[nodiscard]] bool poll_was_interrupted(const int error_code) noexcept
{
    return error_code == EINTR;
}
#endif

/// Converts the requested portable interests into native event flags.
[[nodiscard]] short requested_native_events(const SocketPollEntry &entry) noexcept
{
    short events = 0;
    if (entry.watch_readable)
    {
        events = static_cast<short>(events | native_readable_event);
    }
    if (entry.watch_writable)
    {
        events = static_cast<short>(events | native_writable_event);
    }
    return events;
}

/// Clears results left by an earlier wait before descriptors are reused.
void clear_ready_events(SocketPollEntry &entry) noexcept
{
    entry.readable = false;
    entry.writable = false;
    entry.error = false;
    entry.hangup = false;
    entry.invalid = false;
}

/// Copies native readiness flags back into the portable descriptor.
void store_ready_events(SocketPollEntry &entry, const short native_events) noexcept
{
    entry.readable = (native_events & native_readable_event) != 0;
    entry.writable = (native_events & native_writable_event) != 0;
    entry.error = (native_events & POLLERR) != 0;
    entry.hangup = (native_events & POLLHUP) != 0;
    entry.invalid = (native_events & POLLNVAL) != 0;
}

} // namespace

Result<void, NetworkError> NativeSocketPoller::wait(std::span<SocketPollEntry> entries,
                                                    const NetworkOperation operation)
{
    if (entries.empty())
    {
        return unexpected(NetworkError{operation, NetworkErrorDomain::validation, 1});
    }

    std::vector<NativePollEntry> native_entries;
    native_entries.reserve(entries.size());
    for (auto &entry : entries)
    {
        clear_ready_events(entry);
        native_entries.push_back(NativePollEntry{entry.socket, requested_native_events(entry), 0});
    }

    while (true)
    {
        const int poll_result = poll_native_entries(native_entries);
        if (poll_result >= 0)
        {
            break;
        }

        const int error_code = last_socket_error();
        if (!poll_was_interrupted(error_code))
        {
            return unexpected(NetworkError{operation, NetworkErrorDomain::socket, error_code});
        }
    }

    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        store_ready_events(entries[index], native_entries[index].revents);
    }

    return {};
}

} // namespace sparenode::network::detail
