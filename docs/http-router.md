# HTTP request router

`HttpRouter` maps a validated `HttpMethod` and the path portion of an origin-form request target to
an application handler. Route registration is bounded to 128 entries and must finish before the
router is shared by concurrent connection workers.

Patterns are either exact paths (`/api/status`) or paths with one terminal wildcard
(`/api/file/*`). Exact routes have priority over wildcard routes; otherwise, the longest wildcard
prefix wins. The wildcard suffix is returned as borrowed raw request-path text and is deliberately
not decoded or resolved. A filesystem handler must pass it through the `SafePath` security boundary.
The query component is not part of route matching and remains available through the original
request target.

A successful route match is not an authorization decision. Protected handlers must be composed
with the authentication/authorization layer before registration; the router never calls an
alternative unprotected handler for the same method and path.

When no path matches, dispatch produces `404 Not Found`. When the path exists for another method,
dispatch produces `405 Method Not Allowed` with a bounded `Allow` field. Handler failures remain
structured errors so the future HTTP session layer can log them and select its connection policy.
