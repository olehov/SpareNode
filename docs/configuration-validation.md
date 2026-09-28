# Configuration validation

`ConfigValidator` is the semantic boundary between the parser model and later
runtime configuration mapping. It performs no listener creation and starts no
worker threads. Success produces `ValidatedConfiguration`, which cannot be
constructed directly by unchecked callers. The validated wrapper retains the
canonical `SharedRoot` for each parsed location, so the runtime mapping stage does
not need to reinterpret the original path that was checked.

Validation collects independently detectable failures in deterministic source
traversal order. Each `ConfigValidationError` retains a source location and may
identify the related server directive, location directive, location API path, or detailed
`SharedRootError`. Dependent checks avoid cascading errors: for example, a
missing worker count is reported only after `multithreading true` is known.

## Version-one rules

The validator enforces the semantics documented by the version-one format:

- singleton server and location directives cannot be repeated;
- `bind` accepts only numeric IPv4 and IPv6 text;
- `port` is limited to `1..65535`;
- enabled multithreading requires `worker_threads` in `2..64`;
- disabled or omitted multithreading forbids `worker_threads`;
- `log_level` accepts `debug`, `info`, `warning`, or `error`;
- at least one location with a valid absolute HTTP API path is required;
- duplicate and segment-nested location paths are rejected deterministically;
- each location requires one path accepted and canonicalized by `SharedRoot`;
- read, write, and delete permissions remain independent as specified by SN-080.

The implementation traverses every parsed location, retains its canonical root in
source order, and checks route conflicts on complete path-segment boundaries.
Multiple distinct locations are mapped into the same runtime server configuration.

Unknown names and misplaced directives are rejected earlier by `ConfigParser`.
Filesystem requests made after startup must still pass through `SafePath`;
successful configuration validation does not replace that request boundary.
