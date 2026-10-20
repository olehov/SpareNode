# Module boundaries

SpareNode expresses architectural dependencies through CMake targets. A target
may include public headers from the targets it links, while lower-level targets
must remain independent of higher-level policy and adapters.

The initial dependency boundaries are:

| Target | Owns | Allowed project dependencies |
| --- | --- | --- |
| `sparenode_foundation` | `Result`, `Unexpected`, and `unexpected()` | None |
| `sparenode_logging` | Log records, severity, logger, and sinks | `sparenode_foundation` |
| `sparenode_network` | Sockets, connections, dispatch, and server lifecycle | `sparenode_foundation` |
| `sparenode_storage` | Temporary artifacts, bounded isolated file I/O, and cleanup ownership | `sparenode_foundation` |
| `sparenode_filesystem` | Canonical roots, path containment, filesystem operations, and location permissions | `sparenode_foundation`, `sparenode_storage` |
| `sparenode_network_logging` | Network error formatting and logging observers | `sparenode_logging`, `sparenode_network` |
| `sparenode_configuration` | File loading, lexer, parser, source-located validation, and primitive settings DTOs | `sparenode_foundation` |
| `sparenode_http_core` | HTTP request and response models, parsing, MIME metadata, and routing | `sparenode_foundation`, `sparenode_storage` |
| `sparenode_api_core` | Public/protected route composition and the protected-route policy boundary | `sparenode_http_core` |
| `sparenode_filesystem_application` | Permission enforcement and transport-neutral filesystem use-case orchestration | `sparenode_filesystem`, `sparenode_foundation`, `sparenode_storage` |
| `sparenode_filesystem_api` | Filesystem route registration, focused HTTP handlers, JSON responses, MIME headers, and public error mapping | `sparenode_api_core`, `sparenode_filesystem_application`, `sparenode_http_core` |
| `sparenode_http_transport` | Bounded HTTP connection ingestion and response transmission over TCP | `sparenode_http_core`, `sparenode_network`, `sparenode_storage` |
| `sparenode_core` | Authentication, command-line parsing, and startup of composed server plans | Configuration lexer, foundation, filesystem, HTTP, logging, network, and storage targets |
| `sparenode_bootstrap` | External path resolution and concrete runtime dependency construction | `sparenode_configuration`, transitional `sparenode_core` |

Foundation depends only on the C++ standard library. Logging core must not
include network headers, and network must not include logging headers. The
network logging adapter is selected by the `sparenode` composition executable;
neither domain library selects it for callers.

Filesystem owns `SharedRoot` and `LocationPermissions`. Configuration retains only
decoded path text, permission flags, and source locations. Bootstrap constructs the
filesystem types and performs authentication placement checks before listener startup.

Configuration public headers contain no HTTP, network, logging, filesystem, or
authentication implementation types. MIME registry loading, credential-backed
authentication, endpoint construction, log severity mapping, receive deadlines, and
transport policy mapping belong to bootstrap.

HTTP core is transport-neutral: it does not include or link network code. The
network library knows nothing about HTTP. HTTP transport is the only bridge
between those modules and receives an `HttpApplication` callback rather than a
concrete router. The executable composition root adapts `HttpRouter` to that
callback, so another application implementation can reuse the transport without
changing its connection code.

API core distinguishes public and protected routes when they are registered.
Its protected-route decorator is the composition point for authentication and
other access policy. Filesystem API registers all filesystem operations through
that protected contract and accepts only filesystem locations and MIME mappings;
it does not receive the listener, logging, timeout, or transport settings held by
`ServerConfig`. Each filesystem operation has its own handler translation unit,
so route composition can add or remove one operation without changing unrelated
handlers. HTTP core remains independent of filesystem headers and libraries.

`RunningApplication` receives `ServerRuntimePlan` objects and has no configuration
dependency. Each plan already owns its endpoint, handler, admission limits, and
failure observers, so one server cannot inherit another server's routes, deadlines,
transport policy, or logging threshold. Startup transfers one plan at a time; if a
later server fails, RAII destroys every server started earlier. Bootstrap retains one
process-wide authentication service for the version-one owner realm while the
composition executable builds routing and transport policy independently per server.
The bootstrap plan also exposes the version-one process lifecycle log threshold
explicitly, so process logging does not obtain policy through a first-server lookup.

Storage owns `TemporaryFile`, its cancellation/deadline policy, cleanup contract,
and the process-wide limit of 32 active or orphaned native file operations.
HTTP request ingestion receives a storage factory and uses `TemporaryFileWriter`;
it does not implement file workers or include filesystem operation headers.
Upload finalization consumes the completed storage artifact while retaining all
destination confinement and no-replace publication policy in filesystem.

The filesystem application service owns the list/read decision, upload
materialization and publication, directory creation, deletion, and the single
permission check for each use case. Its result types contain no HTTP statuses or
responses. The filesystem API translates requests into that service contract and
maps application failures into the existing public HTTP representation.

Module-focused test executables link only the target under test and its declared
dependencies. This makes an accidental reverse dependency fail during linking
instead of remaining hidden behind the transitional `sparenode_core` target.
