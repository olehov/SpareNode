# MVP authentication model

SpareNode v0.1 uses one authenticated owner identity to protect every configured filesystem
location. Authentication proves that a request belongs to that owner. Existing location
permissions still decide whether the authenticated owner may read, write, or delete a resource.

This document defines the contract consumed by credential, session, login, browser-security, and
rate-limit work. It does not select a concrete secret source or implement a multi-user permission
system.

## Identity model

The only v0.1 principal is the SpareNode owner. Its stable internal identity is `owner`; a login
name is a credential lookup attribute and is not an authorization role. Application code receives
an authenticated principal containing an opaque identity identifier. It must not branch on an
`isAdmin` flag, compare a username to `owner`, or read credential configuration directly.

Every authenticated v0.1 principal has the same owner identity. Authorization still evaluates the
matched location's `read`, `write`, and `delete` settings. Authentication therefore cannot grant an
operation disabled by location policy.

The identity identifier and principal are deliberately independent of credential storage. A future
provider may return other identity identifiers without changing the HTTP login shape or session
manager, but multiple accounts, roles, ACLs, per-user roots, and private user storage are outside
v0.1.

## Credential-provider boundary

Authentication depends on a `CredentialProvider` abstraction with one logical operation: look up
credential verification material by a submitted login identifier. A successful lookup returns a
record containing:

- an opaque identity identifier;
- the canonical login identifier required for audit-safe internal decisions;
- an encoded password verifier containing its algorithm, parameters, and salt.

The provider never returns a plaintext password. The authentication service passes the submitted
password and encoded verifier to a separate password-verification component. It returns either an
authenticated principal or one generic authentication failure. A missing login runs an equivalent
dummy verification path so response contents and gross timing do not reveal whether the login
identifier exists.

The authentication service does not know whether the provider uses an external secret,
environment variable, operating-system secret store, configuration adapter, or future database.
The initial provider may obtain the owner verifier from an externally supplied secret, but
`spnode.conf` must not contain a plaintext password.

At startup, the v0.1 provider must yield exactly one usable owner credential. Missing, duplicate,
malformed, or unsupported verification material fails startup without logging the login value,
password, verifier, salt, or environment contents. SpareNode does not create a default password and
does not silently start protected routes without usable credentials.

SN-041 selects the vetted password-hashing library and concrete provisioning adapter. Algorithm
selection, parameter upgrades, and secure password setup belong there rather than in the session
or HTTP layers.

## Authentication flow

```text
POST /api/auth/login
        |
        v
bounded, non-persistent credential parsing
        |
        v
authentication service
        |
        +--> CredentialProvider --> encoded verifier + identity
        |
        +--> password verifier --> generic success or failure
        |
        v
fresh server-side session --> opaque session cookie
        |
        v
protected request --> principal --> location permission --> filesystem boundary
```

Credential request bodies use a dedicated small, bounded in-memory path. They must never enter the
temporary-file upload pipeline, crash diagnostics, request tracing, or logs. SN-042 defines the
exact JSON limits and parser, with an initial maximum body size no greater than 4 KiB.

## Endpoint boundary

The v0.1 authentication endpoints are exact routes:

| Endpoint | Access | Behaviour |
| --- | --- | --- |
| `POST /api/auth/login` | Public | Verifies credentials and creates a fresh session. |
| `POST /api/auth/logout` | Session-aware | Invalidates the presented session and expires its cookie. |
| `OPTIONS *` | Public | Preserves the server-wide HTTP capability response and performs no protected operation. |

Every configured filesystem location is protected, including:

- `GET` and implicit `HEAD` directory listing, preview, and download routes;
- `PUT` upload routes;
- `POST` directory-creation routes;
- `DELETE` file and directory routes;
- future filesystem operations registered beneath a configured location.

An endpoint is public only when registration marks it public explicitly. Adding a route must not
make it public by omission. Protocol parsing failures, `OPTIONS *`, unknown-route `404`, and
method-selection `405` responses may be produced before authentication because they invoke no
application resource handler. Once a protected route is selected, authentication happens before
path decoding, location permission checks, filesystem inspection, request-body persistence, or
mutation.

Requests to a selected protected route without a valid session return `401 Unauthorized` with a
stable `{"error":"authentication_required"}` body and
`WWW-Authenticate: SpareNodeSession realm="SpareNode"`. The private challenge name describes the
cookie-backed application session without implying support for HTTP Basic or bearer authorization
headers. An authenticated principal that lacks the required location permission receives
`403 Forbidden`. Neither response discloses native paths or resource existence beyond the routing
surface.

## Login behaviour

The login contract accepts a login identifier and password over the transport permitted by
SN-108. The following rules apply:

1. Parse only the documented bounded media type and schema.
2. Apply SN-044 throttling before an expensive password verification.
3. Verify through the credential-provider and password-verifier boundaries.
4. Return the same `401` response and `{"error":"invalid_credentials"}` body for an unknown login
   and an incorrect password.
5. On success, invalidate any session credential presented with the login request, create a new
   session identifier, and return the session cookie. A client cannot select or preserve a session
   identifier across login.
6. Do not return credential records or identity details beyond what the client needs to establish
   the owner session.

Malformed input returns `400 Bad Request`. Temporarily unavailable credential infrastructure fails
closed with `503 Service Unavailable` and the stable
`{"error":"authentication_unavailable"}` body. The response does not distinguish provider,
verification, or secret-source failures. Passwords, verifier strings, session identifiers, and
cookie values are never included in responses or logs.

SN-107 defines login Origin handling and the final cookie attributes. SN-108 defines when the
transport is sufficiently protected to accept credentials.

## Authentication audit logging

The default MVP audit trail records only successful authentication state changes:

```text
authentication login_success principal=owner peer=192.0.2.10
authentication logout_success principal=owner peer=192.0.2.10
```

`principal` comes from the authenticated credential or session record, never directly from the
submitted login string. `peer` is the direct socket peer unless SN-108 has positively identified a
trusted proxy and derived the effective client address under that policy. Arbitrary forwarding
headers must never replace the logged peer address.

The records contain no password, password verifier, cookie, session identifier, CSRF value,
request body, native path, or filename. Logout is recorded only when it invalidates a live session;
anonymous or repeated logout requests do not produce misleading success events.

Individual failed logins are not logged by default because an unauthenticated client could flood
the operational log or inject sensitive submitted identifiers. SN-044 may emit one bounded,
rate-limited throttling-state event without the submitted login value. IP addresses remain
potentially identifying network metadata even when dynamically assigned, so they receive the same
access and retention protections as other security logs.

## Session lifecycle

A successful login creates a server-side session bound to the authenticated principal. The browser
receives only an opaque credential generated from at least 256 bits of cryptographically secure
randomness and encoded without semantic data. Session identifiers are never accepted from a URL or
request body and never contain an identity, timestamp, permission, or filesystem path.

The server stores a one-way digest of the session credential together with:

- the authenticated principal;
- creation and last-activity times from a monotonic clock;
- an absolute expiration time;
- an idle expiration time;
- server-owned invalidation state where required.

The initial centrally defined policy is a 12-hour absolute lifetime and a 30-minute idle timeout.
A successfully authorized protected request advances idle activity without extending the absolute
deadline. Expiration is checked before authorization. Expired entries are rejected and removed.
The session store is bounded to 64 active entries for the single owner; it removes expired entries
first and then the least recently active entry before admitting a new successful login. These
values may later become validated server settings without changing session semantics.

Sessions are memory-only in v0.1. Process restart, graceful shutdown, explicit logout, or
credential-provider replacement invalidates them. Logout invalidates the presented session before
returning and sends an expired cookie. Repeated logout without a live session remains safe and does
not restore or prolong a session.

Cookie transport is the only browser session mechanism. The baseline cookie is host-only,
`HttpOnly`, scoped to `Path=/`, and non-persistent. The final `Secure` and `SameSite` policy,
cross-site request checks, CSRF evidence, duplicate-cookie handling, and cookie name are defined and
tested by SN-107 together with the effective-HTTPS rules from SN-108. Session credentials must
never be placed in browser local storage.

## Request authorization order

Protected handlers follow one common order:

1. Parse enough HTTP metadata to select the route and enforce transport limits.
2. Validate the session credential and resolve its principal.
3. Apply Origin and CSRF policy where the method can change state.
4. Check the configured location permission for the method.
5. Acquire any SN-106 resource coordination lease.
6. Decode and validate the requested relative path.
7. Enter the existing native filesystem confinement boundary.

Failures release session, request-body, coordination, and filesystem resources through RAII. A
slow or disconnected client cannot extend a session or retain a resource lease beyond the request
deadline.

## Deployment threat assumptions

Authentication does not encrypt passwords, cookies, metadata, or file contents in transit.
SN-108 owns the complete network-exposure contract and its configuration validation:

- loopback-only development may permit direct HTTP under explicit local assumptions;
- plaintext trusted-LAN operation, if supported, is an explicit choice with documented
  confidentiality limitations;
- public Internet exposure requires HTTPS through a trusted TLS terminator or a future native TLS
  implementation;
- forwarding headers from an untrusted client never prove that a request used HTTPS.

SN-107 owns browser-origin, CSRF, credentialed CORS, security-header, and untrusted inline-preview
controls. SN-044 owns bounded login throttling and recovery from temporary lockout. These controls
are required independently; authentication, TLS, CSRF protection, and rate limiting do not replace
one another.

## Security invariants

- The default for every filesystem application route is authenticated.
- A session carries identity only; location configuration remains authoritative for operations.
- Authentication never weakens `SafePath`, native handle confinement, upload publication, or
  deletion safeguards.
- Credential and session secrets are absent from logs, error text, URLs, filesystem names, and
  persisted request bodies.
- Session state, credential request bodies, and rate-limit state have explicit memory bounds.
- All external failures fail closed and expose only stable public error identifiers.
- Windows and Linux use identical authentication and authorization semantics.

## Implementation issue alignment

| Issue | Responsibility under this model |
| --- | --- |
| SN-041 | Credential provider, secret provisioning, vetted password hashing, and verification. |
| SN-043 | Principal types, bounded session store, cookie credential validation, expiry, and protected-route enforcement. |
| SN-042 | Bounded login/logout parsing and responses integrated with SN-041 and SN-043. |
| SN-107 | Cookie hardening, Origin/CSRF policy, CORS restrictions, and preview-origin isolation. |
| SN-044 | Bounded failed-login throttling without permanent owner lockout. |
| SN-108 | Loopback, LAN, HTTPS termination, trusted-proxy, and effective-scheme policy. |
| SN-106 | Read/mutation coordination after authentication and authorization. |

## References

- [NIST SP 800-63B-4, Authentication and Authenticator Management](https://pages.nist.gov/800-63-4/sp800-63b.html)
- [RFC 6265, HTTP State Management Mechanism](https://www.rfc-editor.org/rfc/rfc6265.html)
- [RFC 9110, HTTP Semantics](https://www.rfc-editor.org/rfc/rfc9110.html)
