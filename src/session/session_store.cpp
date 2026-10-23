#include "sparenode/session/session_store.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace sparenode::session
{
namespace
{

/// @brief Bounds retries for an astronomically unlikely generated-digest collision.
constexpr std::size_t maximum_generation_attempts = 4;

} // namespace

Result<std::shared_ptr<SessionStore>, SessionStoreErrorCode>
SessionStore::create(std::shared_ptr<const SessionCredentialCodec> codec, SessionStorePolicy policy,
                     SessionClock clock)
{
    if (codec == nullptr || policy.maximum_sessions == 0 ||
        policy.absolute_lifetime <= std::chrono::steady_clock::duration::zero() ||
        policy.idle_timeout <= std::chrono::steady_clock::duration::zero())
    {
        return unexpected(SessionStoreErrorCode::invalid_policy);
    }
    if (!clock)
    {
        clock = [] { return std::chrono::steady_clock::now(); };
    }
    try
    {
        auto store = std::shared_ptr<SessionStore>(
            new SessionStore(std::move(codec), policy, std::move(clock)));
        store->entries_.reserve(policy.maximum_sessions);
        return store;
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(SessionStoreErrorCode::resource_allocation_failed);
    }
}

SessionStore::SessionStore(std::shared_ptr<const SessionCredentialCodec> codec,
                           const SessionStorePolicy policy, SessionClock clock) noexcept
    : codec_(std::move(codec)), policy_(policy), clock_(std::move(clock))
{
}

bool SessionStore::expired(const Entry &entry,
                           const std::chrono::steady_clock::time_point now) const noexcept
{
    return now - entry.created >= policy_.absolute_lifetime ||
           now - entry.last_activity >= policy_.idle_timeout;
}

bool SessionStore::contains(const SessionCredentialDigest &digest) const noexcept
{
    return std::ranges::any_of(entries_,
                               [&digest](const Entry &entry) { return entry.digest == digest; });
}

void SessionStore::remove_expired(const std::chrono::steady_clock::time_point now)
{
    std::erase_if(entries_, [this, now](const Entry &entry) { return expired(entry, now); });
}

void SessionStore::evict_least_recent_if_full()
{
    if (entries_.size() != policy_.maximum_sessions)
    {
        return;
    }
    const auto least_recent = std::ranges::min_element(entries_, {}, [](const Entry &entry)
                                                       { return entry.last_activity; });
    entries_.erase(least_recent);
}

Result<std::string, SessionStoreErrorCode>
SessionStore::create_session(authentication::Principal principal)
{
    try
    {
        for (std::size_t attempt = 0; attempt < maximum_generation_attempts; ++attempt)
        {
            auto generated = codec_->generate();
            if (!generated)
            {
                return unexpected(SessionStoreErrorCode::credential_generation_failed);
            }

            const auto now = clock_();
            std::lock_guard lock(mutex_);
            remove_expired(now);
            if (contains(generated->digest))
            {
                continue;
            }
            evict_least_recent_if_full();
            entries_.push_back(Entry{generated->digest, std::move(principal), now, now});
            return std::move(generated->value);
        }
        return unexpected(SessionStoreErrorCode::credential_generation_failed);
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(SessionStoreErrorCode::resource_allocation_failed);
    }
}

Result<std::optional<authentication::Principal>, SessionStoreErrorCode>
SessionStore::authenticate(const std::string_view credential)
{
    auto digest = codec_->digest(credential);
    if (!digest)
    {
        if (digest.error() == SessionCredentialErrorCode::invalid_credential)
        {
            return std::optional<authentication::Principal>{};
        }
        return unexpected(SessionStoreErrorCode::credential_digest_failed);
    }
    try
    {
        const auto now = clock_();
        std::lock_guard lock(mutex_);
        remove_expired(now);
        const auto entry = std::ranges::find(entries_, digest.value(), &Entry::digest);
        if (entry == entries_.end())
        {
            return std::optional<authentication::Principal>{};
        }
        entry->last_activity = now;
        return std::optional<authentication::Principal>{entry->principal};
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(SessionStoreErrorCode::resource_allocation_failed);
    }
}

Result<std::optional<authentication::Principal>, SessionStoreErrorCode>
SessionStore::invalidate(const std::string_view credential)
{
    auto digest = codec_->digest(credential);
    if (!digest)
    {
        if (digest.error() == SessionCredentialErrorCode::invalid_credential)
        {
            return std::optional<authentication::Principal>{};
        }
        return unexpected(SessionStoreErrorCode::credential_digest_failed);
    }
    try
    {
        const auto now = clock_();
        std::lock_guard lock(mutex_);
        remove_expired(now);
        const auto entry = std::ranges::find(entries_, digest.value(), &Entry::digest);
        if (entry == entries_.end())
        {
            return std::optional<authentication::Principal>{};
        }
        auto principal = std::optional<authentication::Principal>{entry->principal};
        entries_.erase(entry);
        return principal;
    }
    catch (const std::bad_alloc &)
    {
        return unexpected(SessionStoreErrorCode::resource_allocation_failed);
    }
}

std::size_t SessionStore::size() const
{
    std::lock_guard lock(mutex_);
    return entries_.size();
}

} // namespace sparenode::session
