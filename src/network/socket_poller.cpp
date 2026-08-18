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
/// @brief Native Windows poll descriptor used by the platform adapter.
using NativePollEntry = WSAPOLLFD;
/// @brief Native Windows flag representing read readiness.
inline constexpr short native_readable_event = POLLRDNORM;
/// @brief Native Windows flag representing write readiness.
inline constexpr short native_writable_event = POLLWRNORM;

/// @brief Calls the Windows polling API for a portable entry collection.
/// @param[in,out] entries Native descriptors populated with readiness results.
/// @return Number of ready descriptors, or the native failure sentinel.
[[nodiscard]] int poll_native_entries(std::vector<NativePollEntry> &entries) noexcept
{
    return WSAPoll(entries.data(), static_cast<ULONG>(entries.size()), -1);
}

/// @brief Reports whether Windows polling was interrupted before completion.
/// @param[in] error_code Winsock error code.
/// @return `true` when the poll may be retried.
[[nodiscard]] bool poll_was_interrupted(const int error_code) noexcept
{
    return error_code == WSAEINTR;
}
#else
/// @brief Native POSIX poll descriptor used by the platform adapter.
using NativePollEntry = pollfd;
/// @brief Native POSIX flag representing read readiness.
inline constexpr short native_readable_event = POLLIN;
/// @brief Native POSIX flag representing write readiness.
inline constexpr short native_writable_event = POLLOUT;

/// @brief Calls the POSIX polling API for a portable entry collection.
/// @param[in,out] entries Native descriptors populated with readiness results.
/// @return Number of ready descriptors, or the native failure sentinel.
[[nodiscard]] int poll_native_entries(std::vector<NativePollEntry> &entries) noexcept
{
    return ::poll(entries.data(), static_cast<nfds_t>(entries.size()), -1);
}

/// @brief Reports whether POSIX polling was interrupted before completion.
/// @param[in] error_code errno value reported by poll.
/// @return `true` when the poll may be retried.
[[nodiscard]] bool poll_was_interrupted(const int error_code) noexcept
{
    return error_code == EINTR;
}
#endif

/// @brief Converts requested portable interests into native event flags.
/// @param[in] entry Portable poll entry to inspect.
/// @return Native bit mask of requested events.
[[nodiscard]] short requested_native_events(const SocketPollEntry &entry) noexcept
{
    short events = 0;
    if (entry.watch_readable)
    {
        events = native_readable_event;
    }
    if (entry.watch_writable)
    {
        events = static_cast<short>(events | native_writable_event);
    }
    return events;
}

/// @brief Clears results left by an earlier wait before descriptors are reused.
/// @param[in,out] entry Portable poll entry whose output flags are reset.
void clear_ready_events(SocketPollEntry &entry) noexcept
{
    entry.readable = false;
    entry.writable = false;
    entry.error = false;
    entry.hangup = false;
    entry.invalid = false;
}

/// @brief Copies native readiness flags back into the portable descriptor.
/// @param[in,out] entry Portable poll entry that receives decoded flags.
/// @param[in] native_events Native readiness bit mask to decode.
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
