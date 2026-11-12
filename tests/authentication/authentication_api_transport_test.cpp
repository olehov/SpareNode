#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "sparenode/application/session_authentication_service.hpp"
#include "sparenode/authentication/api/authentication_api.hpp"
#include "sparenode/authentication/credential_provider.hpp"
#include "sparenode/authentication/password_verifier.hpp"
#include "sparenode/http/api/api_router_builder.hpp"
#include "sparenode/http/http_application.hpp"
#include "sparenode/http/transport/http_connection_handler.hpp"
#include "sparenode/session/session_credential_codec.hpp"
#include "sparenode/session/session_store.hpp"
#include "support/connected_tcp_pair.hpp"
#include "support/optional.hpp"

namespace
{

/// @brief Returns one deterministic owner credential for end-to-end HTTP tests.
class TestCredentialProvider final : public sparenode::authentication::CredentialProvider
{
  public:
    /// @brief Returns the deterministic owner only for the expected fixture login.
    [[nodiscard]] sparenode::Result<std::optional<sparenode::authentication::CredentialRecord>,
                                    sparenode::authentication::CredentialProviderError>
    find_by_login(const std::string_view login) const override
    {
        if (login != "admin")
        {
            return std::optional<sparenode::authentication::CredentialRecord>{};
        }
        return std::optional<sparenode::authentication::CredentialRecord>{
            sparenode::authentication::CredentialRecord{"owner", "admin", "encoded"}};
    }
};

/// @brief Verifies the deterministic plaintext used only by transport integration tests.
class TestPasswordVerifier final : public sparenode::authentication::PasswordVerifier
{
  public:
    /// @brief Produces the deterministic verifier used by this fixture.
    [[nodiscard]] sparenode::Result<std::string,
                                    sparenode::authentication::PasswordVerifierErrorCode>
    hash(const std::span<const char>) const override
    {
        return std::string("encoded");
    }

    /// @brief Accepts only the fixture password and deterministic verifier pair.
    [[nodiscard]] sparenode::Result<bool, sparenode::authentication::PasswordVerifierErrorCode>
    verify(const std::span<const char> password,
           const std::string_view encoded_verifier) const noexcept override
    {
        return encoded_verifier == "encoded" &&
               std::string_view(password.data(), password.size()) == "password";
    }

    /// @brief Treats the fixture verifier as usable for transport-focused tests.
    [[nodiscard]] bool is_usable(const std::string_view) const noexcept override
    {
        return true;
    }
};

/// @brief Generates stable opaque session credentials for wire-response assertions.
class TestSessionCodec final : public sparenode::session::SessionCredentialCodec
{
  public:
    /// @brief Generates a unique deterministic credential and its lookup digest.
    [[nodiscard]] sparenode::Result<sparenode::session::GeneratedSessionCredential,
                                    sparenode::session::SessionCredentialErrorCode>
    generate() const override
    {
        auto value = "session-" + std::to_string(sequence_.fetch_add(1));
        return sparenode::session::GeneratedSessionCredential{value, digest_for(value)};
    }

    /// @brief Converts a submitted fixture credential into its lookup digest.
    [[nodiscard]] sparenode::Result<sparenode::session::SessionCredentialDigest,
                                    sparenode::session::SessionCredentialErrorCode>
    digest(const std::string_view credential) const noexcept override
    {
        return digest_for(credential);
    }

  private:
    /// @brief Folds a fixture credential into one deterministic lookup digest.
    [[nodiscard]] static sparenode::session::SessionCredentialDigest
    digest_for(const std::string_view credential) noexcept
    {
        sparenode::session::SessionCredentialDigest digest{};
        for (std::size_t index = 0; index < credential.size(); ++index)
        {
            digest[index % digest.size()] ^= static_cast<std::byte>(credential[index]);
        }
        return digest;
    }

    mutable std::atomic_size_t sequence_{}; ///< Produces a fresh fixture credential per login.
};

/// @brief Process-wide dependencies retained across independent HTTP connections.
struct TestRuntime
{
    std::shared_ptr<sparenode::session::SessionStore> sessions; ///< Active bounded sessions.
    std::shared_ptr<const sparenode::application::SessionAuthenticationService>
        service; ///< Login/logout application boundary.
};

/// @brief Creates deterministic authentication and session dependencies.
[[nodiscard]] TestRuntime make_runtime()
{
    auto authentication = sparenode::authentication::AuthenticationService::create(
        std::make_shared<TestCredentialProvider>(), std::make_shared<TestPasswordVerifier>());
    REQUIRE(authentication);
    auto sessions = sparenode::session::SessionStore::create(std::make_shared<TestSessionCodec>());
    REQUIRE(sessions);
    auto service = sparenode::application::SessionAuthenticationService::create(
        std::move(authentication).value(), sessions.value());
    REQUIRE(service);
    return {sessions.value(),
            std::make_shared<const sparenode::application::SessionAuthenticationService>(
                std::move(service).value())};
}

/// @brief Composes the public authentication routes used by the server.
[[nodiscard]] sparenode::http::HttpRouter make_router(const TestRuntime &runtime)
{
    sparenode::http::api::ApiRouterBuilder builder;
    REQUIRE(sparenode::authentication::api::register_authentication_api_routes(builder,
                                                                               runtime.service));
    return std::move(builder).build();
}

/// @brief Adapts the test router to the transport-neutral HTTP application boundary.
[[nodiscard]] sparenode::http::HttpApplication
application_for(const sparenode::http::HttpRouter &router)
{
    return [&router](const sparenode::http::HttpRequestView &request)
               -> sparenode::Result<sparenode::http::HttpResponse,
                                    sparenode::http::HttpApplicationError>
    {
        auto response = router.dispatch(request);
        if (!response)
        {
            return sparenode::unexpected(sparenode::http::HttpApplicationError{
                sparenode::http::HttpApplicationErrorCode::request_failed,
                response.error().detail});
        }
        return std::move(response).value();
    };
}

/// @brief Sends every request byte across a potentially partial socket transfer.
void send_all(const sparenode::test::TestClientSocket &client, const std::string_view source)
{
    auto remaining = std::as_bytes(std::span(source.data(), source.size()));
    while (!remaining.empty())
    {
        const auto sent = client.send(remaining);
        REQUIRE(sent > 0);
        remaining = remaining.subspan(static_cast<std::size_t>(sent));
    }
}

/// @brief Receives one complete response after the one-request connection closes.
[[nodiscard]] std::string receive_until_closed(const sparenode::test::TestClientSocket &client)
{
    std::string response;
    std::array<std::byte, 512> buffer{};
    while (true)
    {
        const auto received = client.receive_within(buffer, std::chrono::seconds{2});
        const auto count = sparenode::test::require_optional(received);
        REQUIRE(count >= 0);
        if (count == 0)
        {
            return response;
        }
        response.append(reinterpret_cast<const char *>(buffer.data()),
                        static_cast<std::size_t>(count));
    }
}

/// @brief Runs one request through the real connection handler and returns its wire response.
[[nodiscard]] std::string exchange(const sparenode::http::HttpRouter &router,
                                   const sparenode::http::HttpConnectionHandlerConfig &config,
                                   const std::string_view request)
{
    auto pair = sparenode::test::create_connected_tcp_pair();
    send_all(pair.client, request);
    pair.client.shutdown_send();
    REQUIRE(sparenode::http::handle_http_connection(std::move(pair.server), application_for(router),
                                                    {}, config));
    return receive_until_closed(pair.client);
}

/// @brief Adds forwarding metadata whose trust depends on the configured transport policy.
[[nodiscard]] std::string proxy_headers()
{
    return "X-Forwarded-Proto: https\r\nX-Forwarded-For: 192.0.2.25\r\n";
}

} // namespace

TEST_CASE("Logout invalidates sessions through direct HTTP and trusted proxy HTTPS",
          "[authentication][logout][integration][transport]")
{
    const bool proxy = GENERATE(false, true);
    const auto runtime = make_runtime();
    const auto router = make_router(runtime);
    const sparenode::http::HttpConnectionHandlerConfig config{
        .body_storage_policy_selector =
            sparenode::authentication::api::authentication_body_storage_policy,
        .deadline_provider = {},
        .transport_policy =
            proxy ? sparenode::http::
                        HttpTransportPolicy{sparenode::http::HttpTransportMode::trusted_proxy_https,
                                            "127.0.0.1"}
                  : sparenode::http::HttpTransportPolicy{}};

    constexpr std::string_view body = R"({"login":"admin","password":"password"})";
    const std::string login_request =
        "POST /api/auth/login HTTP/1.1\r\nHost: localhost\r\n" + proxy_headers() +
        "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\n\r\n" + std::string(body);
    const auto login_response = exchange(router, config, login_request);
    REQUIRE(login_response.starts_with("HTTP/1.1 204 No Content\r\n"));
    const std::string login_cookie =
        "Set-Cookie: spnode_session=session-0; Path=/; HttpOnly; SameSite=Strict" +
        std::string(proxy ? "; Secure\r\n" : "\r\n");
    CHECK(login_response.contains(login_cookie));

    const std::string malformed_request =
        "POST /api/auth/logout HTTP/1.1\r\nHost: localhost\r\n" + proxy_headers() +
        "X-SpareNode-CSRF: 1\r\n"
        "Cookie: spnode_session=session-0; spnode_session=session-0\r\n"
        "Content-Length: 0\r\n\r\n";
    const auto malformed = exchange(router, config, malformed_request);
    CHECK(malformed.starts_with("HTTP/1.1 204 No Content\r\n"));
    CHECK(!malformed.contains("Set-Cookie:"));
    CHECK(malformed.ends_with("\r\n\r\n"));
    const auto preserved = runtime.sessions->authenticate("session-0");
    REQUIRE(preserved);
    CHECK(sparenode::test::require_optional(preserved.value()).identity == "owner");

    const std::string logout_request =
        "POST /api/auth/logout HTTP/1.1\r\nHost: localhost\r\n" + proxy_headers() +
        "X-SpareNode-CSRF: 1\r\nOrigin: " +
        std::string(proxy ? "https://localhost" : "http://localhost") + "\r\n" +
        "Cookie: spnode_session=session-0\r\nContent-Length: 0\r\n\r\n";
    const auto logout_response = exchange(router, config, logout_request);
    REQUIRE(logout_response.starts_with("HTTP/1.1 204 No Content\r\n"));
    const std::string expired_cookie =
        "Set-Cookie: spnode_session=; Path=/; HttpOnly; SameSite=Strict" +
        std::string(proxy ? "; Secure; Max-Age=0\r\n" : "; Max-Age=0\r\n");
    CHECK(logout_response.contains(expired_cookie));

    const auto invalidated = runtime.sessions->authenticate("session-0");
    REQUIRE(invalidated);
    CHECK(!invalidated->has_value());

    const auto repeated = exchange(router, config, logout_request);
    const std::string anonymous_request =
        "POST /api/auth/logout HTTP/1.1\r\nHost: localhost\r\n" + proxy_headers() +
        "X-SpareNode-CSRF: 1\r\nOrigin: https://cross-site.example\r\n"
        "Content-Length: 0\r\n\r\n";
    const auto anonymous = exchange(router, config, anonymous_request);
    CHECK(repeated == logout_response);
    CHECK(anonymous.starts_with("HTTP/1.1 403 Forbidden\r\n"));
    CHECK(!anonymous.contains("Set-Cookie:"));
    CHECK(anonymous.ends_with("{\"error\":\"csrf_validation_failed\"}"));
    CHECK(!repeated.contains("session-0"));
    CHECK(!anonymous.contains("session-0"));
}
