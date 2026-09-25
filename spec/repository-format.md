# Repository format — milestone 0.9

For development and local testing, a repository is a directory containing installable `.pux` package archives. The resolver also accepts `.manifest` and `.pux.manifest` development files so dependency resolution can be tested without building archives.

Current development layout:

```text
repo/
├── glibc-2.44-1-x86_64.pux
├── libidn2-2.3.7-1-x86_64.pux
└── hello-1.0.0-1-x86_64.pux
```

`pux install <package-name> <repository-dir>` loads package metadata from the directory, resolves dependencies, validates the selected package archives, and executes the resulting plan dependency-first.

The final remote repository format is still open. It is expected to add a signed index, repository identity, package hashes, freshness rules, key rotation, mirrors, and transport URLs. The client will eventually verify repository metadata and package content before installation.


## Repository index (Milestone 0.13 — SHA-256 integrity)

A development repository may contain `index.pux`. It begins with `# pux-index=1` and then contains one blank-line-separated package entry per `.pux` archive:

```text
package=hello-1.0.0-1-x86_64.pux
size=12345
sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
format=1
name=hello
version=1.0.0
release=1
arch=x86_64
description=...
license=GPL-3.0-or-later
depends=...
provides=...

```

The package manifest fields are copied from the package and remain authoritative. `size` is the archive byte size and `sha256` is the lowercase hexadecimal SHA-256 digest of the complete `.pux` file. Both are integrity metadata. `pux repo create` scans and validates every `.pux` archive and writes entries in deterministic package order. `pux repo validate` checks size, SHA-256, and package identity. Repository-aware install and upgrade verify the selected package against the index before changing the target filesystem. Digital signatures, remote transport, freshness, mirrors, and key rotation remain future repository features.
