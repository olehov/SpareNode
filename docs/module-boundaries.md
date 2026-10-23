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
| `sparenode_api_core` | Public/protected route composition and the protected-route policy boundary | `sparenode_authentication_core`, `sparenode_http_core` |
| `sparenode_authentication_core` | Principals, credential and password-verification ports, sensitive input, and authentication use cases | `sparenode_foundation` |
| `sparenode_authentication_sodium` | Libsodium Argon2id implementation of the password-verification port | `sparenode_authentication_core` |
| `sparenode_authentication_file` | Bounded credential parsing, secure file loading, placement validation, and provisioning | `sparenode_authentication_core`, `sparenode_configuration`, `sparenode_filesystem` |
| `sparenode_session_core` | Bounded in-memory session lifecycle behind injected credential cryptography and monotonic time | `sparenode_authentication_core`, `sparenode_foundation` |
| `sparenode_session_application` | Successful-login session creation and explicit logout orchestration | `sparenode_authentication_core`, `sparenode_session_core` |
| `sparenode_session_sodium` | Secure 256-bit session credential generation and one-way lookup digests | `sparenode_session_core` |
| `sparenode_authentication_api` | Cookie-backed session authentication policy for protected API routes | `sparenode_api_core`, `sparenode_session_core` |
| `sparenode_filesystem_application` | Permission enforcement and transport-neutral filesystem use-case orchestration | `sparenode_filesystem`, `sparenode_foundation`, `sparenode_storage` |
| `sparenode_filesystem_api` | Filesystem route registration, focused HTTP handlers, JSON responses, MIME headers, and public error mapping | `sparenode_api_core`, `sparenode_filesystem_application`, `sparenode_http_core` |
| `sparenode_http_transport` | Bounded HTTP connection ingestion and response transmission over TCP | `sparenode_http_core`, `sparenode_network`, `sparenode_storage` |
| `sparenode_command_line` | Startup argument parsing | `sparenode_foundation` |
| `sparenode_runtime_config` | Header-only immutable runtime settings shared by bootstrap and composition | Filesystem, HTTP, logging, and network targets |
| `sparenode_application` | Startup and RAII ownership of fully composed server plans | `sparenode_network` |
| `sparenode_bootstrap` | External path resolution and concrete runtime dependency construction | Authentication adapters, `sparenode_configuration`, `sparenode_runtime_config` |

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

API core distinguishes public and protected routes when they are registered and
retains enumerable descriptors for both groups during composition. Its protected
route decorator evaluates transport, session authentication, Origin/CSRF,
authorization, throttling, and resource-coordination policies in that fixed order
before invoking an operation handler. Filesystem API registers all filesystem
operations through that protected contract and accepts only filesystem locations and MIME mappings;
it does not receive the listener, logging, timeout, or transport settings held by
`ServerConfig`. Each filesystem operation has its own handler translation unit,
so route composition can add or remove one operation without changing unrelated
handlers. HTTP core remains independent of filesystem headers and libraries.

Authentication core depends only on foundation contracts. It owns abstract
credential-provider and password-verification ports, so it contains no HTTP,
configuration, file-access, or libsodium headers. The sodium and file adapters
implement those ports in separate targets. Bootstrap selects both concrete
adapters, verifies that the credential file is outside every shared root, and
hands only the source-independent `AuthenticationService` to runtime composition.
The credential provisioning executable selects the same adapters explicitly.

Session core retains only one-way credential digests and is independent of HTTP,
configuration, files, and libsodium. Its synchronized store applies the centrally
defined 12-hour absolute lifetime, 30-minute idle timeout, and 64-entry bound. The
sodium adapter generates 256-bit opaque credentials, while bootstrap owns the
process-wide application service that joins credential verification to bounded session creation
and logout invalidation. Authentication API translates the
host-only session cookie into the fixed protected-route policy and supplies a
source-independent `Principal` to later authorization stages.

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

Module-focused test executables link only the target under test and any dependency
whose public API the test uses directly. TCP-to-filesystem and authentication
composition scenarios live in dedicated integration executables, platform
confinement remains in the filesystem security suite, and generated-version checks
live in the smoke suite. This makes accidental reverse dependencies fail during
linking instead of remaining hidden behind a broad aggregate target.

Every production target registers its permitted direct first-party dependencies
with `SpareNodeArchitecture.cmake`. Configuration fails when a target adds an
undeclared edge or when registered edges form a cycle. CI also exports CMake's
Graphviz target graph so architecture changes remain reviewable as an artifact.
