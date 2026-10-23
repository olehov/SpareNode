#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "sparenode/authentication/principal.hpp"
#include "sparenode/session/session_store.hpp"
#include "support/optional.hpp"

namespace
{

class DeterministicCodec final : public sparenode::session::SessionCredentialCodec
{
  public:
    [[nodiscard]] sparenode::Result<sparenode::session::GeneratedSessionCredential,
                                    sparenode::session::SessionCredentialErrorCode>
    generate() const override
    {
        const auto sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto value = std::string("credential-") + std::to_string(sequence);
        auto digest = digest_for(value);
        return sparenode::session::GeneratedSessionCredential{value, digest};
    }

    [[nodiscard]] sparenode::Result<sparenode::session::SessionCredentialDigest,
                                    sparenode::session::SessionCredentialErrorCode>
    digest(const std::string_view credential) const override
    {
        if (!credential.starts_with("credential-"))
        {
            return sparenode::unexpected(
                sparenode::session::SessionCredentialErrorCode::invalid_credential);
        }
        return digest_for(credential);
    }

  private:
    [[nodiscard]] static sparenode::session::SessionCredentialDigest
    digest_for(const std::string_view value)
    {
        sparenode::session::SessionCredentialDigest digest{};
        for (std::size_t index = 0; index < value.size(); ++index)
        {
            digest[index % digest.size()] ^= static_cast<std::byte>(value[index]);
        }
        return digest;
    }

    mutable std::atomic_size_t sequence_{};
};

struct TestStore
{
    std::shared_ptr<std::chrono::steady_clock::time_point> now;
    std::shared_ptr<sparenode::session::SessionStore> store;
};

[[nodiscard]] TestStore make_store(const std::size_t maximum_sessions = 64)
{
    auto clock_state = std::make_shared<std::chrono::steady_clock::time_point>();
    auto result =
        sparenode::session::SessionStore::create(std::make_shared<DeterministicCodec>(),
                                                 {.absolute_lifetime = std::chrono::hours{12},
                                                  .idle_timeout = std::chrono::minutes{30},
                                                  .maximum_sessions = maximum_sessions},
                                                 [clock_state] { return *clock_state; });
    REQUIRE(result);
    return {std::move(clock_state), std::move(result).value()};
}

/// @brief Creates a fixed batch on one worker and records successful admissions.
void create_sessions(sparenode::session::SessionStore &store, std::atomic_size_t &created,
                     const std::size_t count)
{
    for (std::size_t session = 0; session < count; ++session)
    {
        if (store.create_session({"owner"}))
        {
            created.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace

TEST_CASE("Session store creates validates and invalidates an opaque credential", "[session]")
{
    auto fixture = make_store();
    auto credential = fixture.store->create_session({"owner"});
    REQUIRE(credential);
    CHECK(fixture.store->size() == 1);

    auto principal = fixture.store->authenticate(credential.value());
    REQUIRE(principal);
    REQUIRE(principal->has_value());
    CHECK(sparenode::test::require_optional(principal.value()).identity == "owner");

    auto invalidated = fixture.store->invalidate(credential.value());
    REQUIRE(invalidated);
    REQUIRE(invalidated->has_value());
    CHECK(sparenode::test::require_optional(invalidated.value()).identity == "owner");
    CHECK(fixture.store->size() == 0);
    REQUIRE(fixture.store->authenticate(credential.value()));
    CHECK_FALSE(fixture.store->authenticate(credential.value())->has_value());
}

TEST_CASE("Session store enforces idle and absolute expiration", "[session][expiration]")
{
    auto fixture = make_store();
    auto idle = fixture.store->create_session({"idle"});
    REQUIRE(idle);
    *fixture.now += std::chrono::minutes{30};
    auto expired_idle = fixture.store->authenticate(idle.value());
    REQUIRE(expired_idle);
    CHECK_FALSE(expired_idle->has_value());

    *fixture.now = {};
    auto absolute = fixture.store->create_session({"absolute"});
    REQUIRE(absolute);
    for (int interval = 0; interval < 35; ++interval)
    {
        *fixture.now += std::chrono::minutes{20};
        auto active = fixture.store->authenticate(absolute.value());
        REQUIRE(active);
        REQUIRE(active->has_value());
    }
    *fixture.now = std::chrono::steady_clock::time_point{} + std::chrono::hours{12};
    auto expired_absolute = fixture.store->authenticate(absolute.value());
    REQUIRE(expired_absolute);
    CHECK_FALSE(expired_absolute->has_value());
}

TEST_CASE("Session store advances idle activity without extending absolute lifetime",
          "[session][expiration]")
{
    auto fixture = make_store();
    auto credential = fixture.store->create_session({"owner"});
    REQUIRE(credential);
    *fixture.now += std::chrono::minutes{20};
    REQUIRE(fixture.store->authenticate(credential.value())->has_value());
    *fixture.now += std::chrono::minutes{20};
    REQUIRE(fixture.store->authenticate(credential.value())->has_value());
    *fixture.now += std::chrono::hours{11} + std::chrono::minutes{20};
    auto expired = fixture.store->authenticate(credential.value());
    REQUIRE(expired);
    CHECK_FALSE(expired->has_value());
}

TEST_CASE("Session store evicts expired then least recently active entries", "[session][bounds]")
{
    auto fixture = make_store(2);
    auto first = fixture.store->create_session({"first"});
    REQUIRE(first);
    *fixture.now += std::chrono::minutes{1};
    auto second = fixture.store->create_session({"second"});
    REQUIRE(second);
    *fixture.now += std::chrono::minutes{1};
    REQUIRE(fixture.store->authenticate(first.value())->has_value());
    *fixture.now += std::chrono::minutes{1};
    auto third = fixture.store->create_session({"third"});
    REQUIRE(third);

    REQUIRE(fixture.store->authenticate(first.value())->has_value());
    CHECK_FALSE(fixture.store->authenticate(second.value())->has_value());
    REQUIRE(fixture.store->authenticate(third.value())->has_value());
    CHECK(fixture.store->size() == 2);
}

TEST_CASE("Session store rejects invalid construction and credentials", "[session]")
{
    auto missing = sparenode::session::SessionStore::create(nullptr);
    REQUIRE_FALSE(missing);
    CHECK(missing.error() == sparenode::session::SessionStoreErrorCode::invalid_policy);

    auto fixture = make_store();
    auto invalid = fixture.store->authenticate("not-a-session");
    REQUIRE(invalid);
    CHECK_FALSE(invalid->has_value());
}

TEST_CASE("Session store remains bounded during concurrent creation", "[session][concurrency]")
{
    constexpr std::size_t maximum_sessions = 64;
    constexpr std::size_t worker_count = 8;
    constexpr std::size_t sessions_per_worker = 16;
    auto fixture = make_store(maximum_sessions);
    std::atomic_size_t created{};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker)
    {
        workers.emplace_back([&fixture, &created]
                             { create_sessions(*fixture.store, created, sessions_per_worker); });
    }
    for (auto &worker : workers)
    {
        worker.join();
    }

    CHECK(created.load(std::memory_order_relaxed) == worker_count * sessions_per_worker);
    CHECK(fixture.store->size() == maximum_sessions);
}
