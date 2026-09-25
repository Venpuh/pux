# `.pux` package format — draft

This specification is intentionally versioned and implemented in small steps.

## Package identity

A package filename is expected to follow:

```text
name-version-release-arch.pux
```

The filename is a presentation/convenience form; the manifest remains authoritative.

## Manifest format 1

The manifest is UTF-8 text made of one `key=value` record per line. Blank lines and lines beginning with `#` are ignored. Keys are ASCII tokens. Values must not contain control characters.

Required fields:

```text
format=1
name=<package name>
version=<version>
release=<positive integer>
arch=<architecture>
description=<human-readable description>
license=<license identifier>
```

List-valued fields may repeat:

```text
depends=<dependency expression>
provides=<capability>
conflicts=<package/capability>
replaces=<package>
```

The parser rejects unknown fields and duplicate scalar fields.

## Container

The Milestone 0.4 `.pux` container is an **uncompressed POSIX ustar archive**. Compression is deliberately deferred until the container and extraction safety rules are stable. A future format revision may wrap the same tar payload in zstd without changing the manifest schema.

The archive currently contains at least:

```text
META/manifest
payload/...
```

## Security

The installer must not allow payload extraction outside the target root. Absolute paths, parent traversal, and unsafe link targets must be rejected. Package content must be verified before a transaction is committed.

## Milestone 0.3 container rules

Only these paths are accepted at the archive root: `META/`, `META/manifest`, `payload/`, and entries below `payload/`. Paths must be relative and must not contain `.` or `..` components. Absolute paths and symbolic/hard links are rejected for now. `META/manifest` must be a single regular file and is authoritative package metadata.


## Milestone 0.4 build rules

`pux build <manifest> <payload-dir> <output.pux>` creates a package from a validated manifest and payload directory. The archive contains `META/manifest`, followed by sorted `payload/...` members. Only regular files and directories are accepted; symbolic links and special files are rejected. Archive member names longer than 100 bytes are rejected in this revision. Header UID/GID and mtime are fixed to zero to make output deterministic for identical inputs.

The builder does not perform extraction or installation.


## Extraction policy

Milestone 0.5 extraction accepts only regular files and directories under `payload/`. Archive paths are relative and may not contain `.` or `..` components. Symbolic and hard links are rejected. Existing regular files are never overwritten. Parent directories are opened relative to a directory file descriptor with `O_NOFOLLOW` to avoid following archive- or destination-created symlinks. Setuid and setgid permission bits are not restored during extraction until package signature/trust policy is implemented.
