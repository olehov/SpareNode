#include "sparenode/network/detail/socket_poller.hpp"

#include <cerrno>
#include <chrono>
#include <limits>
#include <optional>
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
[[nodiscard]] int poll_native_entries(std::vector<NativePollEntry> &entries,
                                      const int timeout_milliseconds) noexcept
{
    return WSAPoll(entries.data(), static_cast<ULONG>(entries.size()), timeout_milliseconds);
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
[[nodiscard]] int poll_native_entries(std::vector<NativePollEntry> &entries,
                                      const int timeout_milliseconds) noexcept
{
    return ::poll(entries.data(), static_cast<nfds_t>(entries.size()), timeout_milliseconds);
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
    const short readable_events = entry.watch_readable ? native_readable_event : short{};
    const short writable_events = entry.watch_writable ? native_writable_event : short{};
    return static_cast<short>(readable_events | writable_events);
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

/// @brief Converts an absolute monotonic deadline to a non-negative native timeout.
/// @param[in] deadline Optional absolute deadline for the current wait.
/// @return `-1` for an unlimited wait, otherwise a ceiling-rounded bounded millisecond count.
[[nodiscard]] int
native_timeout_milliseconds(const std::optional<NetworkDeadline> &deadline) noexcept
{
    if (!deadline.has_value())
    {
        return -1;
    }

    const auto remaining = deadline.value() - std::chrono::steady_clock::now();
    if (remaining <= NetworkDeadline::duration::zero())
    {
        return 0;
    }

    const auto remaining_milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining);
    constexpr auto maximum_native_timeout =
        std::chrono::milliseconds{(std::numeric_limits<int>::max)()};
    if (remaining_milliseconds >= maximum_native_timeout)
    {
        return (std::numeric_limits<int>::max)();
    }
    return static_cast<int>(remaining_milliseconds.count());
}

} // namespace

Result<SocketPollStatus, NetworkError>
NativeSocketPoller::wait(std::span<SocketPollEntry> entries, const NetworkOperation operation,
                         const std::optional<NetworkDeadline> deadline)
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
        const int poll_result =
            poll_native_entries(native_entries, native_timeout_milliseconds(deadline));
        if (poll_result > 0)
        {
            break;
        }
        if (poll_result == 0)
        {
            if (deadline.has_value() && std::chrono::steady_clock::now() < deadline.value())
            {
                continue;
            }
            return SocketPollStatus::timed_out;
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

    return SocketPollStatus::events;
}

} // namespace sparenode::network::detail
