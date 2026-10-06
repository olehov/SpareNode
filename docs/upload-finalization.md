# Upload finalization

`filesystem::finalize_upload` publishes a completed `TemporaryFile` under a
validated relative path in a configured `SharedRoot`. It is the filesystem
operation for SN-034; an HTTP upload route is a separate task.

The caller retains ownership of the completed source. The finalizer validates
the destination with `SafePath`, opens its parent as a confined directory, and
copies the source into an exclusive sibling staging file using a fixed-size
buffer. The staging file is flushed before publication. Linux links the open
staging inode to the final name; Windows renames the open staging handle. Both
operations use the retained parent handle and refuse to replace an existing
entry. This also handles a source temporary file and destination share on
different volumes. The `.sparenode-upload-` prefix is reserved: pending stages
are omitted from directory listings and rejected by direct file reads.

Successful publication leaves the final file in the share. Destroying the
`TemporaryFile` then removes its original temporary path. On cancellation,
deadline expiry, copy or flush failure, or a destination-name conflict, the
final name is not created and the finalizer attempts to remove its staging
file. A failed cleanup can leave an `.sparenode-upload-*.tmp` sibling, which an
administrator may remove after ensuring no upload is active. Cancellation and
deadlines are checked between bounded native operations; a stalled native
filesystem call cannot be forcibly interrupted here.
