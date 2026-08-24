# Safe-path boundary

`sparenode::filesystem::SafePath` is the first type accepted by filesystem
operations for paths originating outside the process. Callers resolve an
untrusted UTF-8 path relative to a validated `SharedRoot`; they cannot directly
construct a `SafePath` from an unrestricted host path.

## Resolution contract

`SafePath::resolve()` performs these steps:

1. Reject requests larger than 4096 bytes before parsing or conversion.
2. Decode URL percent escapes exactly once and reject malformed or ambiguously
   nested encoding.
3. Normalize slash and backslash to one portable separator representation.
4. Validate the decoded path as UTF-8 and reject decoded or literal null bytes.
5. Reject slash-rooted, UNC-style, and Windows drive-qualified input on every
   platform before converting it to `std::filesystem::path`.
6. Convert the decoded request to the platform-native path form.
7. On Windows, reject alternate data stream separators, reserved punctuation,
   control characters, non-special components ending in an ASCII space or
   period, and device names such as `NUL`, `CON`, `COM1`, and their
   extension-bearing aliases before Win32 can reinterpret them.
8. Join the relative request to the canonical shared root.
9. Normalize `.` and `..` components lexically without requiring the target to
   exist.
10. Compare complete path components to ensure the result is the shared root or
   one of its descendants.

URL path decoding is case-insensitive for hexadecimal digits and does not treat
`+` as a space. A decoded result containing another valid percent escape is
rejected instead of decoded repeatedly. This prevents validation from approving
a path that a later accidental decoding pass could reinterpret as traversal.

Both `/` and `\` are network path separators regardless of the host operating
system. Consequently, mixed-separator traversal has the same result on Windows
and Linux, and Windows drive or UNC syntax cannot become an ordinary Linux file
name at this boundary.

The 4096-byte limit is a SpareNode input policy that bounds work performed on
untrusted data. It is not a statement of the host filesystem's maximum path
length, and successful resolution does not guarantee that every later native
filesystem operation will accept the resulting path.

Containment does not use a string-prefix check. For example, a sibling named
`shared-private` is not considered a child of a root named `shared`.

An empty request resolves to the shared root. A missing final path component is
also allowed when it remains inside the root, enabling later upload and create
operations. The operation consuming `SafePath` remains responsible for its own
requirements, such as whether the target must already exist or be a regular
file.

## Errors

Resolution returns `SafePathError` rather than throwing for expected invalid
input. The error distinguishes an oversized request, invalid or nested percent
encoding, invalid UTF-8, embedded null bytes, rooted input, platform-invalid
components, and escape from the shared root. Oversized input is not copied into
the error object; its `requested_path` field is empty.

## Remaining security work

`SafePath` establishes lexical confinement only. It deliberately does not
follow symbolic links or inspect Windows reparse points because a missing upload
destination must also be representable. Dedicated issues extend this boundary
with symbolic-link protection, Windows reparse-point handling, and
operation-time safeguards against filesystem races.
