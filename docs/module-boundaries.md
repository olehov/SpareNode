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

Foundation depends only on the C++ standard library. Logging core must not
include network headers, and network must not include logging headers. The
network logging adapter is selected by the `sparenode` composition executable;
neither domain library selects it for callers.

Filesystem owns `SharedRoot` and `LocationPermissions`. Configuration validates
and constructs these types but filesystem code does not include configuration
headers. Authentication placement checks consume canonical shared roots rather
than runtime location configuration.

Storage owns `TemporaryFile`, its cancellation/deadline policy, cleanup contract,
and the process-wide limit of 32 active or orphaned native file operations.
HTTP request ingestion receives a storage factory and uses `TemporaryFileWriter`;
it does not implement file workers or include filesystem operation headers.
Upload finalization consumes the completed storage artifact while retaining all
destination confinement and no-replace publication policy in filesystem.

Module-focused test executables link only the target under test and its declared
dependencies. This makes an accidental reverse dependency fail during linking
instead of remaining hidden behind the transitional `sparenode_core` target.
