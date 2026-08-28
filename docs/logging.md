# Application logging

SpareNode uses one application-owned logging abstraction for startup,
configuration, networking, and shutdown diagnostics. Production code emits
structured `LogRecord` values through `Logger`; only `ConsoleLogSink` knows how
to write them to a terminal. Domain errors remain structured return values and
observers, so logging never replaces error propagation.

Each terminal record is one UTC line:

```text
[2026-08-24T12:34:56.123Z] [INFO] [application] SpareNode startup complete
```

The fields are timestamp, severity, subsystem, and message. Supported severity
levels are `debug`, `info`, `warning`, and `error`.

On an interactive terminal, `ConsoleLogSink` colors only the severity marker:
`DEBUG` is cyan, `INFO` is green, `WARNING` is yellow, and `ERROR` is red.
Redirected output remains plain text, so files and pipelines never receive ANSI
escape sequences. Tests and specialized callers can explicitly enable or disable
coloring through `ConsoleColorMode`.

Startup failures can occur before the application constructs its configured
`Logger`. `write_console_diagnostic()` preserves their source-oriented layout
while applying the same color policy to labels such as `error:`.

## Configuration

`SPARENODE_LOG_LEVEL` controls the minimum emitted severity:

```dotenv
SPARENODE_LOG_LEVEL=info
```

Values are case-sensitive. Omitting the variable safely defaults to `info`.
An empty or unsupported value prevents startup with a structured configuration
error. Records below the selected level are discarded before timestamp or
message storage is allocated.

## Concurrency and failure containment

Copies of a `Logger` share one synchronization state. A complete record is
forwarded to its sink under that shared mutex. `ConsoleLogSink` also protects
its stream, preventing interleaving when independent loggers share the same
console sink.

`Logger::log()` is `noexcept` and contains allocation and sink exceptions.
Network logging adapters also contain message-formatting failures before they
can cross dispatcher-worker or accept-thread boundaries. Logging therefore
cannot change the result of the operation being diagnosed.

Tests inject an in-memory sink rather than reading terminal output. This keeps
formatting, filtering, concurrency, and failure-containment tests deterministic.

## Network error adapters

`format_network_error()` preserves a `NetworkError` operation, domain, and
numeric code. `make_connection_failure_log_observer()` and
`make_connection_server_failure_log_observer()` adapt the existing dispatcher
and server observer APIs to logging without adding logger dependencies to the
network domain interfaces.

## Security and privacy

Messages and subsystem names are treated as data, never as format strings.
Newlines, carriage returns, tabs, backslashes, and other ASCII control bytes are
escaped before console output, preventing untrusted input from forging extra
records. Initial lifecycle integration does not log environment values, shared
directory paths, credentials, tokens, session identifiers, or file contents.

File output, rotation, retention, remote shipping, metrics, and tracing remain
outside the MVP logging scope.
