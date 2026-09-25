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

The planned `.pux` container is a compressed POSIX tar archive. The initial target remains zstd compression, but container parsing is deliberately separate from manifest parsing so that the metadata format can be tested independently.

The archive will contain at least:

```text
META/manifest
payload/...
```

## Security

The installer must not allow payload extraction outside the target root. Absolute paths, parent traversal, and unsafe link targets must be rejected. Package content must be verified before a transaction is committed.
