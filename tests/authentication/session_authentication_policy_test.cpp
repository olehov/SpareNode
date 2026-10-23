#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "sparenode/authentication/api/session_authentication_policy.hpp"
#include "sparenode/http/api/api_router_builder.hpp"
#include "sparenode/http/request/http_request_parser.hpp"
#include "support/optional.hpp"

namespace
{

class TestCodec final : public sparenode::session::SessionCredentialCodec
{
  public:
    /// @brief Configures whether digest derivation simulates unavailable cryptography.
    explicit TestCodec(const bool fail_digest = false) : fail_digest_(fail_digest)
    {
    }

    /// @brief Returns the deterministic credential used by policy tests.
    [[nodiscard]] sparenode::Result<sparenode::session::GeneratedSessionCredential,
                                    sparenode::session::SessionCredentialErrorCode>
    generate() const override
    {
        return sparenode::session::GeneratedSessionCredential{"credential", digest_value()};
    }

    /// @brief Derives the deterministic digest or reports the configured failure.
    [[nodiscard]] sparenode::Result<sparenode::session::SessionCredentialDigest,
                                    sparenode::session::SessionCredentialErrorCode>
    digest(const std::string_view credential) const override
    {
        if (fail_digest_)
        {
            return sparenode::unexpected(
                sparenode::session::SessionCredentialErrorCode::cryptography_unavailable);
        }
        if (credential != "credential")
        {
            return sparenode::unexpected(
                sparenode::session::SessionCredentialErrorCode::invalid_credential);
        }
        return digest_value();
    }

  private:
    /// @brief Builds the stable digest shared by generated and submitted credentials.
    [[nodiscard]] static sparenode::session::SessionCredentialDigest digest_value()
    {
        sparenode::session::SessionCredentialDigest digest{};
        digest.front() = std::byte{0x2A};
        return digest;
    }

    bool fail_digest_{};
};

/// @brief Parses one complete HTTP request for route-policy tests.
[[nodiscard]] sparenode::http::HttpRequestView parse_request(const std::string_view source)
{
    auto parsed =
        sparenode::http::parse_http_request(std::as_bytes(std::span(source.data(), source.size())));
    REQUIRE(parsed);
    return sparenode::test::require_optional(parsed->request());
}

/// @brief Converts the owned response bytes into comparable text.
[[nodiscard]] std::string body_text(const sparenode::http::HttpResponse &response)
{
    const auto body = response.memory_body();
    return {reinterpret_cast<const char *>(body.data()), body.size()};
}

/// @brief Creates a session store backed by the deterministic test codec.
[[nodiscard]] std::shared_ptr<sparenode::session::SessionStore>
make_store(const bool fail_digest = false)
{
    auto store = sparenode::session::SessionStore::create(std::make_shared<TestCodec>(fail_digest));
    REQUIRE(store);
    return std::move(store).value();
}

struct TestObservations
{
    bool handler_called{};
    bool principal_seen{};
};

/// @brief Records whether authentication supplied a principal before dispatch.
[[nodiscard]] sparenode::http::api::ApiRequestDecorator
observe_principal(TestObservations &observations)
{
    return [&observations](sparenode::http::api::ApiRequestHandler next)
    {
        return sparenode::http::api::ApiRequestHandler(
            [&observations,
             next = std::move(next)](sparenode::http::api::ApiRequestContext &context)
            {
                observations.principal_seen = context.authenticated_principal.has_value() &&
                                              context.authenticated_principal->identity == "owner";
                return next(context);
            });
    };
}

/// @brief Creates a terminal route handler that records successful dispatch.
[[nodiscard]] sparenode::http::HttpRouteHandler successful_handler(TestObservations &observations)
{
    return [&observations](const sparenode::http::HttpRequestView &,
                           const sparenode::http::HttpRouteParameters &)
               -> sparenode::Result<sparenode::http::HttpResponse, sparenode::http::HttpRouteError>
    {
        observations.handler_called = true;
        auto response = sparenode::http::HttpResponse::create(sparenode::http::HttpStatusCode::ok,
                                                              "OK", {}, {});
        REQUIRE(response);
        return std::move(response).value();
    };
}

/// @brief Builds one protected test router using the supplied session store.
[[nodiscard]] sparenode::http::HttpRouter
make_router(const std::shared_ptr<sparenode::session::SessionStore> &store,
            TestObservations &observations)
{
    sparenode::http::api::ProtectedRoutePolicies policies;
    policies.session_authentication =
        sparenode::authentication::api::make_session_authentication_policy(store);
    policies.authorization = observe_principal(observations);
    sparenode::http::api::ApiRouterBuilder builder(
        sparenode::http::api::make_protected_route_decorator(std::move(policies)));
    REQUIRE(builder.register_protected(
        {sparenode::http::HttpMethod::get, "/files", successful_handler(observations)}));
    return std::move(builder).build();
}

/// @brief Verifies that unavailable session infrastructure fails closed before dispatch.
void verify_unavailable_policy(const std::shared_ptr<sparenode::session::SessionStore> &store)
{
    TestObservations observations;
    auto router = make_router(store, observations);
    auto response = router.dispatch(parse_request(
        "GET /files HTTP/1.1\r\nHost: localhost\r\nCookie: spnode_session=credential\r\n\r\n"));
    REQUIRE(response);
    CHECK(response->status_code() == sparenode::http::HttpStatusCode::service_unavailable);
    CHECK(body_text(response.value()) == "{\"error\":\"authentication_unavailable\"}");
    CHECK(observations.handler_called == false);
    CHECK(observations.principal_seen == false);
}

} // namespace

TEST_CASE("Session policy rejects missing malformed and invalid cookies",
          "[authentication][session]")
{
    auto store = make_store();
    TestObservations observations;
    auto router = make_router(store, observations);

    for (const std::string request :
         {"GET /files HTTP/1.1\r\nHost: localhost\r\n\r\n",
          "GET /files HTTP/1.1\r\nHost: localhost\r\nCookie: spnode_session=invalid\r\n\r\n",
          "GET /files HTTP/1.1\r\nHost: localhost\r\nCookie: spnode_session=\r\n\r\n",
          "GET /files HTTP/1.1\r\nHost: localhost\r\nCookie: "
          "spnode_session_backup=credential\r\n\r\n",
          "GET /files HTTP/1.1\r\nHost: localhost\r\nCookie: theme=dark\r\nCookie: "
          "spnode_session=credential\r\n\r\n",
          "GET /files HTTP/1.1\r\nHost: localhost\r\nCookie: spnode_session=credential; "
          "spnode_session=credential\r\n\r\n"})
    {
        auto response = router.dispatch(parse_request(request));
        REQUIRE(response);
        CHECK(response->status_code() == sparenode::http::HttpStatusCode::unauthorized);
        CHECK(body_text(response.value()) == "{\"error\":\"authentication_required\"}");
        REQUIRE(response->headers().size() == 2);
        CHECK(response->headers()[1].name == "WWW-Authenticate");
        CHECK(response->headers()[1].value == "SpareNodeSession realm=\"SpareNode\"");
    }
    CHECK(observations.handler_called == false);
    CHECK(observations.principal_seen == false);
}

TEST_CASE("Session policy fails closed when session infrastructure is unavailable",
          "[authentication][session]")
{
    verify_unavailable_policy({});
    verify_unavailable_policy(make_store(true));
}

TEST_CASE("Session policy rejects expired and invalidated sessions before dispatch",
          "[authentication][session]")
{
    auto now = std::make_shared<std::chrono::steady_clock::time_point>();
    auto created =
        sparenode::session::SessionStore::create(std::make_shared<TestCodec>(),
                                                 {.absolute_lifetime = std::chrono::hours{12},
                                                  .idle_timeout = std::chrono::minutes{30},
                                                  .maximum_sessions = 2},
                                                 [now] { return *now; });
    REQUIRE(created);
    auto store = std::move(created).value();
    auto credential = store->create_session({"owner"});
    REQUIRE(credential);

    TestObservations expired_observations;
    auto expired_router = make_router(store, expired_observations);
    *now += std::chrono::minutes{30};
    auto expired = expired_router.dispatch(parse_request(
        "GET /files HTTP/1.1\r\nHost: localhost\r\nCookie: spnode_session=credential\r\n\r\n"));
    REQUIRE(expired);
    CHECK(expired->status_code() == sparenode::http::HttpStatusCode::unauthorized);
    CHECK_FALSE(expired_observations.handler_called);

    *now = {};
    credential = store->create_session({"owner"});
    REQUIRE(credential);
    REQUIRE(store->invalidate(credential.value()));
    TestObservations invalidated_observations;
    auto invalidated_router = make_router(store, invalidated_observations);
    auto invalidated = invalidated_router.dispatch(parse_request(
        "GET /files HTTP/1.1\r\nHost: localhost\r\nCookie: spnode_session=credential\r\n\r\n"));
    REQUIRE(invalidated);
    CHECK(invalidated->status_code() == sparenode::http::HttpStatusCode::unauthorized);
    CHECK_FALSE(invalidated_observations.handler_called);
}

TEST_CASE("Session policy supplies the authenticated principal before the handler",
          "[authentication][session]")
{
    auto store = make_store();
    auto credential = store->create_session({"owner"});
    REQUIRE(credential);
    TestObservations observations;
    auto router = make_router(store, observations);
    auto response = router.dispatch(
        parse_request("GET /files HTTP/1.1\r\nHost: localhost\r\ncOoKiE: theme=dark; "
                      "spnode_session=credential\r\n\r\n"));
    REQUIRE(response);
    CHECK(response->status_code() == sparenode::http::HttpStatusCode::ok);
    CHECK(observations.handler_called);
    CHECK(observations.principal_seen);
}
