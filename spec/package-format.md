# `.pux` package format — draft

This is an initial design draft, not a stable specification.

## Container

A `.pux` package is planned as a compressed POSIX tar archive. The initial target is zstd compression once the archive implementation is added.

The archive contains at least:

```text
META/manifest
payload/...
```

Optional package metadata or build records may be added later under `META/`.

## Manifest

The manifest is UTF-8 text using strict `key=value` records. Repeated list-valued keys are permitted.

Example:

```text
format=1
name=hello
version=1.0.0
release=1
arch=x86_64
description=GNU Hello example package
license=GPL-3.0-or-later
depends=glibc>=2.44
```

The stable specification will define escaping, ordering, comparison rules, dependency expressions, and allowed fields before the first stable release.

## Safety requirements

The installer must not allow a package to write outside the target root. Absolute paths, parent traversal, and unsafe symlink/hardlink targets must be rejected or normalized according to the final security specification.

## Integrity and authenticity

Repository metadata and packages are expected to support cryptographic hashes and signatures. The signature scheme will be selected before the repository protocol is declared stable.
