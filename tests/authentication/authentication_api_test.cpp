#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
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
#include "sparenode/http/request/http_request_parser.hpp"
#include "sparenode/session/session_credential_codec.hpp"
#include "sparenode/session/session_store.hpp"
#include "support/optional.hpp"

namespace
{

class TestCredentialProvider final : public sparenode::authentication::CredentialProvider
{
  public:
    explicit TestCredentialProvider(const bool unavailable = false) : unavailable_(unavailable)
    {
    }

    [[nodiscard]] sparenode::Result<std::optional<sparenode::authentication::CredentialRecord>,
                                    sparenode::authentication::CredentialProviderError>
    find_by_login(const std::string_view login) const override
    {
        ++calls;
        if (unavailable_)
        {
            return sparenode::unexpected(sparenode::authentication::CredentialProviderError{});
        }
        if (login != "admin")
        {
            return std::optional<sparenode::authentication::CredentialRecord>{};
        }
        return std::optional<sparenode::authentication::CredentialRecord>{
            sparenode::authentication::CredentialRecord{"owner", "admin", "encoded"}};
    }

    mutable std::atomic_size_t calls{};

  private:
    bool unavailable_{};
};

class TestPasswordVerifier final : public sparenode::authentication::PasswordVerifier
{
  public:
    [[nodiscard]] sparenode::Result<std::string,
                                    sparenode::authentication::PasswordVerifierErrorCode>
    hash(const std::span<const char>) const override
    {
        return std::string("encoded");
    }

    [[nodiscard]] sparenode::Result<bool, sparenode::authentication::PasswordVerifierErrorCode>
    verify(const std::span<const char> password,
           const std::string_view encoded_verifier) const noexcept override
    {
        ++calls;
        return encoded_verifier == "encoded" &&
               std::string_view(password.data(), password.size()) == "password";
    }

    [[nodiscard]] bool is_usable(const std::string_view) const noexcept override
    {
        return true;
    }

    mutable std::atomic_size_t calls{};
};

class TestSessionCodec final : public sparenode::session::SessionCredentialCodec
{
  public:
    [[nodiscard]] sparenode::Result<sparenode::session::GeneratedSessionCredential,
                                    sparenode::session::SessionCredentialErrorCode>
    generate() const override
    {
        auto value = "session-" + std::to_string(sequence_.fetch_add(1));
        return sparenode::session::GeneratedSessionCredential{value, digest_for(value)};
    }

    [[nodiscard]] sparenode::Result<sparenode::session::SessionCredentialDigest,
                                    sparenode::session::SessionCredentialErrorCode>
    digest(const std::string_view credential) const noexcept override
    {
        return digest_for(credential);
    }

  private:
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

    mutable std::atomic_size_t sequence_{};
};

struct Fixture
{
    std::shared_ptr<TestPasswordVerifier> verifier;
    std::shared_ptr<sparenode::session::SessionStore> sessions;
    std::shared_ptr<const sparenode::application::SessionAuthenticationService> service;
};

[[nodiscard]] Fixture make_fixture(const bool unavailable = false)
{
    auto provider = std::make_shared<TestCredentialProvider>(unavailable);
    auto verifier = std::make_shared<TestPasswordVerifier>();
    auto authentication =
        sparenode::authentication::AuthenticationService::create(provider, verifier);
    REQUIRE(authentication);
    auto sessions = sparenode::session::SessionStore::create(std::make_shared<TestSessionCodec>());
    REQUIRE(sessions);
    auto service = sparenode::application::SessionAuthenticationService::create(
        std::move(authentication).value(), sessions.value());
    REQUIRE(service);
    return {std::move(verifier), std::move(sessions).value(),
            std::make_shared<const sparenode::application::SessionAuthenticationService>(
                std::move(service).value())};
}

[[nodiscard]] sparenode::http::HttpRequestView parse_request(const std::string_view source)
{
    auto parsed =
        sparenode::http::parse_http_request(std::as_bytes(std::span(source.data(), source.size())));
    REQUIRE(parsed);
    REQUIRE(parsed->is_complete());
    return sparenode::test::require_optional(parsed->request());
}

[[nodiscard]] sparenode::http::HttpResponse
dispatch_login(const sparenode::http::HttpRouter &router, const std::string_view body,
               const std::string_view extra_headers = "Content-Type: application/json\r\n")
{
    const std::string source =
        "POST /api/auth/login HTTP/1.1\r\nHost: localhost\r\n" + std::string(extra_headers) +
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
    auto response = router.dispatch(parse_request(source));
    REQUIRE(response);
    return std::move(response).value();
}

[[nodiscard]] std::string body_text(const sparenode::http::HttpResponse &response)
{
    const auto body = response.memory_body();
    return {reinterpret_cast<const char *>(body.data()), body.size()};
}

[[nodiscard]] std::string_view header_value(const sparenode::http::HttpResponse &response,
                                            const std::string_view name)
{
    const auto headers = response.headers();
    const auto match = std::ranges::find(headers, name, &sparenode::http::HttpResponseHeader::name);
    return match == headers.end() ? std::string_view{} : std::string_view(match->value);
}

[[nodiscard]] sparenode::http::HttpRouter make_router(const Fixture &fixture)
{
    sparenode::http::api::ApiRouterBuilder builder;
    auto registered = sparenode::authentication::api::register_authentication_api_routes(
        builder, fixture.service);
    REQUIRE(registered);
    REQUIRE(builder.public_routes().size() == 1);
    CHECK(builder.protected_routes().empty());
    return std::move(builder).build();
}

} // namespace

TEST_CASE("Authentication API creates and rotates owner sessions", "[authentication][api]")
{
    const auto fixture = make_fixture();
    const auto router = make_router(fixture);
    constexpr std::string_view credentials = R"({"login":"admin","password":"password"})";

    const auto first = dispatch_login(router, credentials);
    REQUIRE(first.status_code() == sparenode::http::HttpStatusCode::no_content);
    const std::string first_cookie(header_value(first, "Set-Cookie"));
    REQUIRE(first_cookie.starts_with("spnode_session=session-0;"));

    const auto rejected =
        dispatch_login(router, R"({"login":"admin","password":"incorrect"})",
                       "Content-Type: application/json\r\nCookie: spnode_session=session-0\r\n");
    CHECK(rejected.status_code() == sparenode::http::HttpStatusCode::unauthorized);
    const auto retained_session = fixture.sessions->authenticate("session-0");
    REQUIRE(retained_session);
    REQUIRE(retained_session->has_value());

    const auto second = dispatch_login(
        router, credentials,
        "Content-Type: application/json; charset=UTF-8\r\nCookie: spnode_session=session-0\r\n");
    REQUIRE(second.status_code() == sparenode::http::HttpStatusCode::no_content);
    CHECK(header_value(second, "Set-Cookie").starts_with("spnode_session=session-1;"));

    const auto old_session = fixture.sessions->authenticate("session-0");
    REQUIRE(old_session);
    CHECK_FALSE(old_session->has_value());
    const auto new_session = fixture.sessions->authenticate("session-1");
    REQUIRE(new_session);
    CHECK(sparenode::test::require_optional(new_session.value()).identity == "owner");
}

TEST_CASE("Authentication API does not distinguish invalid credentials", "[authentication][api]")
{
    const auto fixture = make_fixture();
    const auto router = make_router(fixture);

    const auto incorrect = dispatch_login(router, R"({"login":"admin","password":"incorrect"})");
    const auto unknown = dispatch_login(router, R"({"login":"unknown","password":"incorrect"})");

    CHECK(incorrect.status_code() == sparenode::http::HttpStatusCode::unauthorized);
    CHECK(unknown.status_code() == sparenode::http::HttpStatusCode::unauthorized);
    CHECK(body_text(incorrect) == R"({"error":"invalid_credentials"})");
    CHECK(body_text(unknown) == body_text(incorrect));
}

TEST_CASE("Authentication API reports a bounded failed-login cooldown", "[authentication][api]")
{
    const auto fixture = make_fixture();
    const auto router = make_router(fixture);
    constexpr std::string_view incorrect = R"({"login":"admin","password":"incorrect"})";

    const sparenode::authentication::AuthenticationRateLimitPolicy policy;
    for (std::size_t attempt = 0; attempt < policy.maximum_failures; ++attempt)
    {
        const auto response = dispatch_login(router, incorrect);
        REQUIRE(response.status_code() == sparenode::http::HttpStatusCode::unauthorized);
    }

    const auto throttled = dispatch_login(router, R"({"login":"admin","password":"password"})");
    CHECK(throttled.status_code() == sparenode::http::HttpStatusCode::too_many_requests);
    CHECK(body_text(throttled) == R"({"error":"authentication_throttled"})");
}

TEST_CASE("Authentication API rejects malformed input before verification", "[authentication][api]")
{
    const auto fixture = make_fixture();
    const auto router = make_router(fixture);
    const auto initial_calls = fixture.verifier->calls.load();

    for (const std::string_view body :
         {"", R"({"login":"admin"})", R"({"password":"password"})",
          R"({"login":"admin","password":"password","extra":"value"})",
          R"({"login":"admin","login":"other","password":"password"})",
          R"({"login":7,"password":"password"})", R"({"login":"admin",})",
          R"({"login":"admin","password":"\uDC00"})", R"({"login":"admin","password":"\u12"})"})
    {
        const auto response = dispatch_login(router, body);
        CHECK(response.status_code() == sparenode::http::HttpStatusCode::bad_request);
        CHECK(body_text(response) == R"({"error":"invalid_login_request"})");
    }
    CHECK(fixture.verifier->calls.load() == initial_calls);

    const auto media = dispatch_login(router, R"({"login":"admin","password":"password"})",
                                      "Content-Type: text/plain\r\n");
    CHECK(media.status_code() == sparenode::http::HttpStatusCode::unsupported_media_type);
    CHECK(fixture.verifier->calls.load() == initial_calls);

    std::string invalid_utf8 = R"({"login":"admin","password":")";
    invalid_utf8.push_back(static_cast<char>(0xC0));
    invalid_utf8 += R"("})";
    const auto encoding = dispatch_login(router, invalid_utf8);
    CHECK(encoding.status_code() == sparenode::http::HttpStatusCode::bad_request);
    CHECK(fixture.verifier->calls.load() == initial_calls);

    const std::string exact_limit(sparenode::authentication::api::maximum_login_body_bytes, ' ');
    const auto exact = dispatch_login(router, exact_limit);
    CHECK(exact.status_code() == sparenode::http::HttpStatusCode::bad_request);

    const std::string oversized(sparenode::authentication::api::maximum_login_body_bytes + 1, ' ');
    const auto too_large = dispatch_login(router, oversized);
    CHECK(too_large.status_code() == sparenode::http::HttpStatusCode::bad_request);
    CHECK(fixture.verifier->calls.load() == initial_calls);
}

TEST_CASE("Authentication API rejects ambiguous media and session fields", "[authentication][api]")
{
    const auto fixture = make_fixture();
    const auto router = make_router(fixture);
    constexpr std::string_view credentials = R"({"login":"admin","password":"password"})";

    for (const std::string_view headers :
         {"", "Content-Type: application/json\r\nContent-Type: application/json\r\n",
          "Content-Type: application/json; charset=iso-8859-1\r\n"})
    {
        const auto response = dispatch_login(router, credentials, headers);
        CHECK(response.status_code() == sparenode::http::HttpStatusCode::unsupported_media_type);
    }
    CHECK(fixture.verifier->calls.load() == 0);

    const auto established = dispatch_login(router, credentials);
    REQUIRE(established.status_code() == sparenode::http::HttpStatusCode::no_content);
    const auto calls_after_login = fixture.verifier->calls.load();

    for (const std::string_view headers :
         {"Content-Type: application/json\r\nCookie: spnode_session=session-0\r\nCookie: "
          "other=value\r\n",
          "Content-Type: application/json\r\nCookie: spnode_session=session-0; "
          "spnode_session=session-0\r\n"})
    {
        const auto response = dispatch_login(router, credentials, headers);
        CHECK(response.status_code() == sparenode::http::HttpStatusCode::bad_request);
        const auto retained = fixture.sessions->authenticate("session-0");
        REQUIRE(retained);
        CHECK(retained->has_value());
    }
    CHECK(fixture.verifier->calls.load() == calls_after_login);
}

TEST_CASE("Authentication body storage policy matches only the login route",
          "[authentication][api]")
{
    using sparenode::authentication::api::authentication_body_storage_policy;
    using sparenode::http::HttpMethod;
    using sparenode::http::HttpRequestHead;
    using sparenode::http::RequestBodyStorageMode;

    const auto login = authentication_body_storage_policy(
        HttpRequestHead{.method = HttpMethod::post, .target = "/api/auth/login?source=test"});
    CHECK(login.mode == RequestBodyStorageMode::memory);
    CHECK(login.max_body_bytes == sparenode::authentication::api::maximum_login_body_bytes);

    const auto wrong_method = authentication_body_storage_policy(
        HttpRequestHead{.method = HttpMethod::put, .target = "/api/auth/login"});
    CHECK(wrong_method.mode == RequestBodyStorageMode::temporary_file);
    CHECK(wrong_method.max_body_bytes == 0);

    const auto wrong_path = authentication_body_storage_policy(
        HttpRequestHead{.method = HttpMethod::post, .target = "/api/auth/logout"});
    CHECK(wrong_path.mode == RequestBodyStorageMode::temporary_file);
    CHECK(wrong_path.max_body_bytes == 0);
}

TEST_CASE("Authentication API fails closed when credential infrastructure is unavailable",
          "[authentication][api]")
{
    const auto fixture = make_fixture(true);
    const auto router = make_router(fixture);

    const auto response = dispatch_login(router, R"({"login":"admin","password":"password"})");

    CHECK(response.status_code() == sparenode::http::HttpStatusCode::service_unavailable);
    CHECK(body_text(response) == R"({"error":"authentication_unavailable"})");
}

TEST_CASE("Authentication API rejects missing composition dependencies", "[authentication][api]")
{
    sparenode::http::api::ApiRouterBuilder builder;
    const auto registered =
        sparenode::authentication::api::register_authentication_api_routes(builder, nullptr);
    REQUIRE_FALSE(registered);
    CHECK(registered.error().code ==
          sparenode::authentication::api::AuthenticationApiErrorCode::missing_service);
}
