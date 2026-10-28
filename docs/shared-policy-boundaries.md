# Shared policy ownership and intentional boundaries

This document records the authoritative owners created by SN-124 and the similar-looking code that
must remain separate because its contract differs. It prevents later maintenance from reintroducing
parallel protocol rules or merging security-sensitive behavior only because its implementation looks
similar.

## Consolidated owners

| Policy | Authoritative owner | Consumers |
| --- | --- | --- |
| HTTP ASCII case, token, field-value, and optional-whitespace rules | `http/detail/ascii.hpp` | request parsing, chunk decoding, response validation, MIME parsing, authentication headers/cookies, and `Expect` handling |
| Canonical HTTP reason phrases | `http/http_status_code.hpp` | router, transport-generated failures, authentication, and filesystem APIs |
| JSON API response construction and route-error mapping | `http/api/json_response.hpp` | authentication and filesystem APIs |
| Terminal-safe diagnostic byte escaping | `foundation/text/diagnostic_escape.hpp` | configuration/bootstrap paths and logging |
| Cancellation-first/deadline-second inspection | `foundation/concurrency/interruption.hpp` | storage, request ingestion, upload finalization, and network waits |
| Strict numeric IPv4/IPv6 parsing | `foundation/text/numeric_ip_address.hpp` | configuration validation and HTTP authority validation |
| File-read size and stable error text | `filesystem/file_read_stream.cpp` | both native file-stream implementations |
| Upload copy bounds, staging retry bound, and interruption mapping | `filesystem/detail/upload_finalization_policy.hpp` | common, Linux, and Windows upload finalization |
| Non-optimizable erasure of ordinary byte and character storage | `foundation/memory/secure_erase.hpp` | authentication plaintext and buffered HTTP request bodies |

## Intentionally separate contracts

Diagnostic paths preserve backslashes and printable bytes so a reported native path remains
recognizable. Log fields additionally quote backslashes and use readable `\\n`, `\\r`, and `\\t`
forms. Both policies share the same control-byte encoder but expose their difference through
`DiagnosticEscapeStyle`.

HTTP authority parsing continues to own DNS labels, brackets, and ports. The shared numeric-address
parser accepts only an address without brackets, a port, or an IPv6 zone identifier. Configuration
adds loopback and wildcard exposure policy after parsing; HTTP adds authority grammar and hostname
policy before dispatch.

Native filesystem opening, containment checks, and system-error translation remain platform
specific. Linux relies on descriptors, `openat`, `linkat`, and `EINTR` retry behavior. Windows relies
on handles, reparse-point checks, handle-relative NT operations, and Win32 error categories. Moving
those paths behind a common path-based implementation would weaken the descriptor/handle identity
guarantees. Only behavior independent of those guarantees is shared.

The file-read validation sequence and its exception boundary also remain in each native translation
unit. Their `bad_alloc` and `filesystem_error` tails look alike, but the guarded operations and the
meaning of a native system error differ. A generic callback wrapper would separate those failures
from the descriptor/handle operation that produced them without consolidating any policy. Captured
file size and stable public error text, which are genuinely platform-neutral, have one common owner.

The Linux and Windows upload copy loops remain separate. POSIX reads and writes are signed,
interruptible, and may require `EINTR` retry; Win32 uses bounded `DWORD` transfers and reports errors
through `GetLastError`. Buffer size, staging attempts, and interruption priority are common policy.

Session credential arrays remain erased with `sodium_memzero` inside the libsodium adapter. This
preserves the cryptographic dependency's explicit guarantee without making foundation depend on
libsodium. Ordinary `char` and `std::byte` storage uses the foundation secure-erasure primitive.

## Test support

Temporary filesystem trees and connected TCP pairs already use the shared fixtures in
`tests/support`. Authentication and session doubles remain beside their scenarios: they intentionally
model different failure injection, credential formats, counters, and concurrency behavior. A single
configurable test double would hide those scenario contracts and make the tests harder to read.

