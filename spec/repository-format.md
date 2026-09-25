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


## Repository index (Milestone 0.12)

A development repository may contain `index.pux`. It begins with `# pux-index=1` and then contains one blank-line-separated package entry per `.pux` archive:

```text
package=hello-1.0.0-1-x86_64.pux
size=12345
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

The package manifest fields are copied from the package and remain authoritative. `size` is the archive byte size used to detect stale index entries. `pux repo create` scans and validates every `.pux` archive and writes entries in deterministic package order. `pux repo validate` validates the index and checks each referenced archive's size and identity. `pux search <term> <repository-dir>` searches name and description through the index. Network transport, cryptographic hashes, signatures, mirrors, freshness and key rotation remain future repository features.
