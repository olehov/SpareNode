# Safe-path boundary

`sparenode::filesystem::SafePath` is the first type accepted by filesystem
operations for paths originating outside the process. Callers resolve an
untrusted UTF-8 path relative to a validated `SharedRoot`; they cannot directly
construct a `SafePath` from an unrestricted host path.

## Resolution contract

`SafePath::resolve()` performs these steps:

1. Reject requests larger than 4096 UTF-8 bytes before parsing or conversion.
2. Validate the requested path as UTF-8 and reject embedded null bytes.
3. Convert the request to the platform-native `std::filesystem::path` form.
4. Reject absolute, rooted, drive-qualified, and UNC-style paths according to
   the host platform's path semantics.
5. On Windows, reject alternate data stream separators, reserved punctuation,
   control characters, non-special components ending in an ASCII space or
   period, and device names such as `NUL`, `CON`, `COM1`, and their
   extension-bearing aliases before Win32 can reinterpret them.
6. Join the relative request to the canonical shared root.
7. Normalize `.` and `..` components lexically without requiring the target to
   exist.
8. Compare complete path components to ensure the result is the shared root or
   one of its descendants.

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
input. The error distinguishes an oversized request, invalid UTF-8, embedded
null bytes, rooted input, platform-invalid components, and escape from the
shared root. Oversized input is not copied into the error object; its
`requested_path` field is empty.

## Remaining security work

`SafePath` establishes lexical confinement only. It deliberately does not
follow symbolic links or inspect Windows reparse points because a missing upload
destination must also be representable. Dedicated issues extend this boundary
with URL and mixed-separator traversal handling, symbolic-link protection,
Windows reparse-point handling, and operation-time safeguards against
filesystem races.
