# Runtime configuration

The runtime configuration is SpareNode's boundary between persistent configuration
syntax and application behavior. The normal configuration pipeline is:

```text
ConfigLexer -> ConfigParser -> ConfigValidator -> RuntimeConfigMapper -> AppConfig
```

`RuntimeConfigMapper` accepts only `ValidatedConfiguration`. Consequently, the normal
startup pipeline cannot map unchecked parser output, and runtime consumers never need
tokens, directive kinds, source locations, or original configuration text.

## Model

`runtime::AppConfig` owns an ordered collection of `runtime::ServerConfig` values.
Version one maps exactly one server, while the collection keeps the runtime API stable
if a later grammar permits multiple server blocks. Each server configuration contains:

- a numeric `network::TcpEndpoint` whose port has already passed the configured
  non-zero `std::uint16_t` range check;
- the multithreading switch and configured worker count;
- the minimum `logging::LogSeverity`;
- an ordered collection of `runtime::ShareConfig` values.

Each share contains its decoded display name, a canonical `SharedRoot`, and independent
`allow_read`, `allow_write`, and `allow_delete` permissions. Filesystem and HTTP code can
therefore enforce permissions without interpreting parser directives or validating the
share path again.

The aggregate runtime types remain convenient for tests and a future settings UI to
construct programmatically. The persistent-file startup path still uses the validator
and mapper to preserve the file format's semantic guarantees.

## Version-one defaults

Omitted directives map to these values:

| Setting | Runtime value |
| --- | --- |
| bind | `0.0.0.0` |
| port | `8080` |
| multithreading | `false` |
| worker threads | `1` |
| log level | `info` |
| read permission | `true` |
| write permission | `false` |
| delete permission | `false` |

When multithreading is disabled, `ServerConfig::effective_worker_count()` returns one.
When it is enabled, it returns the validated `worker_threads` value.
