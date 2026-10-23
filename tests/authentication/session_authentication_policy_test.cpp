#include <catch2/catch_test_macros.hpp>

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
    [[nodiscard]] sparenode::Result<sparenode::session::GeneratedSessionCredential,
                                    sparenode::session::SessionCredentialErrorCode>
    generate() const override
    {
        return sparenode::session::GeneratedSessionCredential{"credential", digest_value()};
    }

    [[nodiscard]] sparenode::Result<sparenode::session::SessionCredentialDigest,
                                    sparenode::session::SessionCredentialErrorCode>
    digest(const std::string_view credential) const override
    {
        if (credential != "credential")
        {
            return sparenode::unexpected(
                sparenode::session::SessionCredentialErrorCode::invalid_credential);
        }
        return digest_value();
    }

  private:
    [[nodiscard]] static sparenode::session::SessionCredentialDigest digest_value()
    {
        sparenode::session::SessionCredentialDigest digest{};
        digest.front() = std::byte{0x2A};
        return digest;
    }
};

[[nodiscard]] sparenode::http::HttpRequestView parse_request(const std::string_view source)
{
    auto parsed =
        sparenode::http::parse_http_request(std::as_bytes(std::span(source.data(), source.size())));
    REQUIRE(parsed);
    return sparenode::test::require_optional(parsed->request());
}

[[nodiscard]] std::string body_text(const sparenode::http::HttpResponse &response)
{
    const auto body = response.memory_body();
    return {reinterpret_cast<const char *>(body.data()), body.size()};
}

[[nodiscard]] std::shared_ptr<sparenode::session::SessionStore> make_store()
{
    auto store = sparenode::session::SessionStore::create(std::make_shared<TestCodec>());
    REQUIRE(store);
    return std::move(store).value();
}

struct TestObservations
{
    bool handler_called{};
    bool principal_seen{};
};

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

TEST_CASE("Session policy supplies the authenticated principal before the handler",
          "[authentication][session]")
{
    auto store = make_store();
    auto credential = store->create_session({"owner"});
    REQUIRE(credential);
    TestObservations observations;
    auto router = make_router(store, observations);
    auto response = router.dispatch(
        parse_request("GET /files HTTP/1.1\r\nHost: localhost\r\nCookie: theme=dark; "
                      "spnode_session=credential\r\n\r\n"));
    REQUIRE(response);
    CHECK(response->status_code() == sparenode::http::HttpStatusCode::ok);
    CHECK(observations.handler_called);
    CHECK(observations.principal_seen);
}
