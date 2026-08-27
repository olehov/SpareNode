# Configuration validation

`ConfigValidator` is the semantic boundary between the parser model and later
runtime configuration mapping. It performs no listener creation and starts no
worker threads. Success produces `ValidatedConfiguration`, which cannot be
constructed directly by unchecked callers. The validated wrapper retains the
canonical `SharedRoot` for each parsed share, so the runtime mapping stage does
not need to reinterpret the original path that was checked.

Validation collects independently detectable failures in deterministic source
traversal order. Each `ConfigValidationError` retains a source location and may
identify the related server directive, share directive, share name, or detailed
`SharedRootError`. Dependent checks avoid cascading errors: for example, a
missing worker count is reported only after `multithreading true` is known.

## Version-one rules

The validator enforces the semantics documented by the version-one format:

- singleton server and share directives cannot be repeated;
- `bind` accepts only numeric IPv4 and IPv6 text;
- `port` is limited to `1..65535`;
- enabled multithreading requires `worker_threads` in `2..64`;
- disabled or omitted multithreading forbids `worker_threads`;
- `log_level` accepts `debug`, `info`, `warning`, or `error`;
- exactly one non-empty named share is required;
- each share requires one path accepted and canonicalized by `SharedRoot`;
- read, write, and delete permissions remain independent as specified by SN-080.

The implementation traverses and validates every parsed share and detects
duplicate names even though version one also reports a second share as a
cardinality failure. This keeps the internal validation architecture ready for
a later multi-share format: removing the version-one cardinality rule will not
require rewriting per-share validation.

Unknown names and misplaced directives are rejected earlier by `ConfigParser`.
Filesystem requests made after startup must still pass through `SafePath`;
successful configuration validation does not replace that request boundary.
