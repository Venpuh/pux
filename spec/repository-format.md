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


## Repository signatures (Milestone 0.14 — Ed25519)

The repository index can be authenticated with a detached Ed25519 signature. The signed bytes are the complete `index.pux` file exactly as stored; the signature does not cover `index.pux.sig` itself.

The signature file is `index.pux.sig` and uses this format:

```text
# pux-ed25519-signature=1
algorithm=ed25519
keyid=<64 lowercase hex characters>
signature=<128 lowercase hex characters>
```

The key identifier is the lowercase SHA-256 digest of the 32-byte raw Ed25519 public key. A public key file uses `# pux-ed25519-public=1`; a private key file uses `# pux-ed25519-private=1` and is created with mode `0600`.

`pux keygen <private-key> <public-key>` creates a keypair. `pux repo sign <repository-dir> <private-key>` signs `index.pux`; `pux repo verify <repository-dir> <public-key>` verifies that detached signature. The repository signature is an additional authenticity layer; the existing per-package SHA-256 remains responsible for package-content integrity.

The current client command does not yet define a system-wide trusted-key store or automatically require signed indexes during install. That trust-policy layer is the next milestone.


## Trusted keys (Milestone 0.15)

A trusted key store contains files named `<keyid>.pub`, where `<keyid>` is the lowercase SHA-256 digest of the 32-byte Ed25519 public key. The default store is `/etc/pux/trusted-keys`; development and tests may override it with `PUX_TRUSTED_KEYS_ROOT`. `pux trust add` validates the public-key file and refuses to overwrite an existing trusted key. `pux repo verify-trusted` obtains the signer keyid from `index.pux.sig`, requires the matching trusted key from the store, and verifies the signature. With `PUX_REQUIRE_SIGNED_REPOSITORY=1`, repository-aware install and upgrade require this trust verification before resolving or installing packages.
