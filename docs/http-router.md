# HTTP request router

`HttpRouter` maps a validated `HttpMethod` and the path portion of an origin-form request target to
an application handler. Route registration is bounded to 128 entries and must finish before the
router is shared by concurrent connection workers.

Patterns are either exact paths (`/api/status`) or paths with one terminal wildcard
(`/api/file/*`). Exact routes have priority over wildcard routes; otherwise, the longest wildcard
prefix wins. The wildcard suffix is returned as borrowed raw request-path text and is deliberately
not decoded or resolved. A filesystem handler must pass it through the `SafePath` security boundary.
The query component is not part of route matching and remains available through the original
normalized request target. Equivalent origin-form and absolute-form targets select
the same path and wildcard suffix. The parser supplies the absolute URL authority
as the effective Host when present.

HEAD can use GET routes without a separate registration. Eligible GET and HEAD
routes are ranked by the same path specificity: exact first, then longest wildcard.
At equal specificity, an explicit HEAD route wins independently of registration
order. Thus a broad HEAD wildcard cannot replace a more specific GET route.
The chosen handler receives the original HEAD method, query, fields, and wildcard
suffix. There is no second handler invocation if it returns an error or denies access.
An explicit HEAD route must apply the same authorization and representation metadata
policy as its corresponding GET route. `Allow` includes HEAD wherever GET is available;
a HEAD-only registration does not implicitly enable GET.

`OPTIONS *` is a built-in server-wide availability request returning an empty
`204 No Content` response, including when no routes are registered. It bypasses
resource routes, including OPTIONS wildcard handlers. This response does not
advertise per-resource method capabilities; ordinary OPTIONS paths still dispatch
through the route table.

A successful route match is not an authorization decision. Protected handlers must be composed
with the authentication/authorization layer before registration; the router never calls an
alternative unprotected handler for the same method and path.

When no path matches, dispatch produces `404 Not Found`. When the path exists for another method,
dispatch produces `405 Method Not Allowed` with a bounded `Allow` field. Handler failures remain
structured errors so the future HTTP session layer can log them and select its connection policy.
