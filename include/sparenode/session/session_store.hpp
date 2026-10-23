#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sparenode/authentication/principal.hpp"
#include "sparenode/foundation/result.hpp"
#include "sparenode/session/session_credential_codec.hpp"

namespace sparenode::session
{

/// @brief Central bounded lifetime and capacity policy for owner sessions.
struct SessionStorePolicy
{
    std::chrono::steady_clock::duration absolute_lifetime{
        std::chrono::hours{12}}; ///< Maximum lifetime from creation.
    std::chrono::steady_clock::duration idle_timeout{
        std::chrono::minutes{30}};    ///< Maximum lifetime since valid activity.
    std::size_t maximum_sessions{64}; ///< Maximum retained live entries.
};

/// @brief Supplies monotonic time and permits deterministic lifecycle tests.
using SessionClock = std::function<std::chrono::steady_clock::time_point()>;

/// @brief Identifies a bounded session-store failure.
enum class SessionStoreErrorCode : std::uint8_t
{
    invalid_policy,               ///< Lifetimes, capacity, or a required dependency is invalid.
    credential_generation_failed, ///< A fresh cryptographic credential could not be produced.
    credential_digest_failed,     ///< A submitted credential could not be safely digested.
    resource_allocation_failed,   ///< Bounded session state could not be allocated or copied.
};

/// @brief Owns bounded in-memory sessions and retains only credential digests.
class SessionStore final
{
  public:
    /// @brief Constructs a synchronized store from injected cryptography and time.
    /// @param[in] codec Concurrent credential generator and digest adapter.
    /// @param[in] policy Fixed lifetime and capacity limits.
    /// @param[in] clock Optional monotonic clock override.
    /// @return Shared store, or a policy or allocation failure.
    [[nodiscard]] static Result<std::shared_ptr<SessionStore>, SessionStoreErrorCode>
    create(std::shared_ptr<const SessionCredentialCodec> codec, SessionStorePolicy policy = {},
           SessionClock clock = {});

    /// @brief Disables copying of synchronized session state.
    SessionStore(const SessionStore &) = delete;
    /// @brief Disables copy assignment of synchronized session state.
    /// @return This store; unavailable because the operation is deleted.
    SessionStore &operator=(const SessionStore &) = delete;
    /// @brief Disables moving of synchronized session state.
    SessionStore(SessionStore &&) = delete;
    /// @brief Disables move assignment of synchronized session state.
    /// @return This store; unavailable because the operation is deleted.
    SessionStore &operator=(SessionStore &&) = delete;
    /// @brief Releases all in-memory session state.
    ~SessionStore() = default;

    /// @brief Creates one session and returns its non-persistent client credential.
    /// @param[in] principal Authenticated identity retained by the session.
    /// @return Fresh client credential, or a bounded store failure.
    [[nodiscard]] Result<std::string, SessionStoreErrorCode>
    create_session(authentication::Principal principal);

    /// @brief Validates a credential, advances idle activity, and returns its principal.
    /// @param[in] credential Opaque credential supplied by the client.
    /// @return Principal when live, empty optional when invalid, or an infrastructure failure.
    [[nodiscard]] Result<std::optional<authentication::Principal>, SessionStoreErrorCode>
    authenticate(std::string_view credential);

    /// @brief Removes a live session and returns its principal when invalidation occurred.
    /// @param[in] credential Opaque credential supplied by the client.
    /// @return Removed principal, empty optional when absent, or an infrastructure failure.
    [[nodiscard]] Result<std::optional<authentication::Principal>, SessionStoreErrorCode>
    invalidate(std::string_view credential);

    /// @brief Returns the current bounded entry count for diagnostics and tests.
    /// @return Number of retained live or not-yet-cleaned expired entries.
    [[nodiscard]] std::size_t size() const;

  private:
    /// @brief Retains one authenticated identity without retaining its client credential.
    struct Entry
    {
        SessionCredentialDigest digest{};                    ///< Only retained credential material.
        authentication::Principal principal;                 ///< Identity established during login.
        std::chrono::steady_clock::time_point created;       ///< Absolute-lifetime anchor.
        std::chrono::steady_clock::time_point last_activity; ///< Idle-lifetime anchor.
    };

    /// @brief Stores validated dependencies and policy after capacity reservation.
    /// @param[in] codec Concurrent credential generator and digest adapter.
    /// @param[in] policy Fixed lifetime and capacity limits.
    /// @param[in] clock Monotonic clock provider.
    SessionStore(std::shared_ptr<const SessionCredentialCodec> codec, SessionStorePolicy policy,
                 SessionClock clock) noexcept;

    /// @brief Tests both lifetime boundaries for one retained entry.
    /// @param[in] entry Candidate session entry.
    /// @param[in] now Current monotonic time.
    /// @return `true` when either lifetime has elapsed.
    [[nodiscard]] bool expired(const Entry &entry,
                               std::chrono::steady_clock::time_point now) const noexcept;
    /// @brief Tests whether a generated digest already exists while the store is locked.
    /// @param[in] digest Candidate digest.
    /// @return `true` when an entry already owns the digest.
    [[nodiscard]] bool contains(const SessionCredentialDigest &digest) const noexcept;
    /// @brief Erases every expired entry while the store is locked.
    /// @param[in] now Current monotonic time.
    void remove_expired(std::chrono::steady_clock::time_point now);
    /// @brief Removes the least recently active entry when capacity is full.
    void evict_least_recent_if_full();

    std::shared_ptr<const SessionCredentialCodec> codec_; ///< Cryptographic boundary.
    SessionStorePolicy policy_;                           ///< Fixed bounded lifecycle policy.
    SessionClock clock_;                                  ///< Monotonic time provider.
    mutable std::mutex mutex_;                            ///< Protects every entry transition.
    std::vector<Entry> entries_;                          ///< At most `maximum_sessions` entries.
};

} // namespace sparenode::session
