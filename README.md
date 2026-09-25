# pux

Milestone 0.20.0-dev adds safe symbolic-link support across package build, validation, extraction, database ownership, and transactions, and includes the first real BLFS 13.1-systemd package definition for GNU nano 9.2.

Milestone 0.19.0-dev adds configured repository selection for package installation and upgrades. After `pux update`, package names can be installed and upgraded without passing repository URLs or cache directories; repository priority, enabled state, and per-repository signature requirements are honored.

The project is intentionally split into a distribution-independent core and a thin Venpux integration layer. Development and testing can therefore happen on ordinary Linux systems before integration into Venpux.

## Current status

The development core now includes deterministic package creation, safe extraction, a persistent package database, dependency resolution, install/remove/upgrade transactions, repository indexes, package SHA-256 verification, Ed25519 repository signatures, a trusted-key store, HTTP(S) metadata updates, remote package installation, and named repository configuration.

Implemented:
- self-contained SHA-256 package checksums and repository hash verification;

- C17 CLI;
- reproducible Makefile build;
- versioned manifest format 1;
- parsing of scalar and repeated metadata fields;
- duplicate/unknown-field rejection;
- manifest validation;
- package `validate` and `info` commands;
- deterministic `build <manifest> <payload-dir> <output.pux>` command;
- safe `package extract <package.pux> <destination>` command;
- persistent package database records with atomic replacement;
- dependency expressions with version constraints;
- read-only dependency resolution with architecture, provides, conflict, and transitive dependency handling;
- `list` and installed-package `info` queries;
- automated CLI, manifest, container, package-build, extraction, database, resolver, install, remove, repository-install, and upgrade tests.

Not implemented yet:

- cross-package transaction rollback;
- repository/package cache garbage collection and configurable retention;
- native HTTP(S) transport without the system `curl` backend.

## Build

Requirements: a C17-capable compiler and POSIX shell utilities.

```sh
make
make test
./build/pux version
./build/pux package validate samples/hello.pux.manifest
./build/pux package info samples/hello.pux.manifest
./build/pux resolve hello samples/repository
```

## Design goals

- native Linux implementation in C;
- deterministic and transactional package operations;
- cryptographically verifiable repositories and packages;
- safe filesystem handling;
- explicit dependency resolution;
- no dependency on Debian, RPM, pacman, or another package manager;
- a small core with Venpux-specific integration isolated behind clear interfaces.

### Package container

Milestone 0.4 reads and writes an uncompressed POSIX ustar `.pux` package containing `META/manifest` and `payload/`. The command `pux build <manifest> <payload-dir> <output.pux>` validates its manifest, collects only regular files and directories, sorts payload entries for deterministic output, and emits fixed metadata. `pux package validate <file>` validates the resulting archive. Extraction uses fd-relative filesystem operations with `O_NOFOLLOW`, rejects path traversal and links, refuses file overwrite, and strips setuid/setgid bits until a package trust policy exists.


## Development status

### 0.8.0-dev

The local transaction engine adds `pux install <package.pux>`. It validates the package, checks architecture and declared dependencies against the installed package database, stages extraction on the target filesystem, performs destination and ownership preflight checks, commits payload files, and records the installed package. `PUX_ROOT` and `PUX_DB_ROOT` can be used for isolated test roots.

### 0.9.0-dev

Repository-aware installation adds `pux install <package-name> <repository-dir>`. The resolver now accepts `.pux` archives as repository candidates and returns an ordered package plan. The CLI validates the whole plan before making changes, skips exact versions already installed, and executes the plan dependency-first using the existing per-package transaction engine. Cross-package rollback and native repository transport are still future work.

## Removal

`pux remove <package-name>` removes an installed package using the ownership information in the package database. Removal is blocked when an installed package would lose a required dependency. Regular files are staged before the database record is removed so the operation can roll back on database failure; package-owned directories are removed only when empty and not recorded by another package.

### 0.12.1-dev

`pux upgrade <package-name> <repository-dir>` resolves the requested package from the repository, installs missing plan dependencies, and replaces installed packages only when the selected repository version is newer. The replacement transaction validates the new package, checks its dependencies, checks installed reverse dependencies and conflicts, stages old files, installs the new payload, and atomically replaces the package database record. Single-package rollback is supported; repository-wide multi-package rollback remains future work.

Upgrade tests cover a normal version replacement, an idempotent no-op, an exact-version dependent that blocks an upgrade, and a compatible dependent that permits it.

## SHA-256 repository integrity — milestone 0.13

Repository indexes now record a SHA-256 digest for every `.pux` archive. `pux repo validate` recomputes each digest and rejects modified package contents even when the archive size is unchanged. Repository-aware `install` and `upgrade` verify the selected archive against the index before starting package transactions. The SHA-256 implementation is self-contained so the package manager does not require an external crypto library just for package checksums.


### Repository signing

Generate a development repository keypair:

```sh
pux keygen ~/.config/pux/repo-signing.key ~/.config/pux/repo-signing.pub
```

Sign an index:

```sh
pux repo sign /path/to/repository ~/.config/pux/repo-signing.key
```

Verify it:

```sh
pux repo verify /path/to/repository ~/.config/pux/repo-signing.pub
```

The current signature layer uses OpenSSL Ed25519 via the EVP interface.


## Trusted repository keys (Milestone 0.15)

Trusted Ed25519 public keys are stored one-per-file under `/etc/pux/trusted-keys` by default. The root can be overridden with `PUX_TRUSTED_KEYS_ROOT` for development and tests. `pux trust add <public-key>` validates and installs a key named by its 64-character SHA-256 keyid; `pux trust list` lists trusted keyids; `pux trust remove <keyid>` revokes a key.

`pux repo verify-trusted <repository-dir>` reads the keyid from `index.pux.sig`, requires the corresponding trusted key, and then verifies the detached Ed25519 signature. Setting `PUX_REQUIRE_SIGNED_REPOSITORY=1` makes repository-aware `install` and `upgrade` require a valid signature from a trusted key before dependency resolution. The default development mode remains backward-compatible and does not require a signature yet.


## Remote repository update (Milestone 0.16)

`pux update <repository-url> <local-repository-dir>` downloads `index.pux` and, when present, `index.pux.sig` over HTTP(S) using the system `curl` executable. The index is parsed and validated before replacement. With `PUX_REQUIRE_SIGNED_REPOSITORY=1`, the detached signature must be issued by a trusted Ed25519 key. Metadata is staged in a private directory and replaced only after validation. The native transport layer is intentionally deferred; the external curl backend is the current development transport.

Milestone 0.17 adds remote package installation from HTTP(S) repositories with trusted-index verification and per-package SHA-256 verification before installation.

## Configured repositories — milestone 0.18

Named repositories are stored as small configuration files under `PUX_REPO_CONFIG_ROOT` (default `/etc/pux/repos.d`). The local metadata/package cache is under `PUX_REPO_CACHE_ROOT` (default `/var/cache/pux/repos`). Each repository has a name, URL, priority, enabled flag, and optional per-repository signature requirement.

Use `pux repo add <name> <url> [priority] [enabled] [require-signature]`, `pux repo list`, and `pux repo remove <name>` to manage entries. `pux repo update <name>` updates one configured repository; `pux update` refreshes all enabled configured repositories. The legacy `pux update <url> <directory>` form remains supported for development compatibility.

`pux search <term>` searches enabled configured repositories, while `pux search <term> <directory>` retains the explicit-directory form. Repository configuration is intentionally separate from the package manager's transaction logic so the later remote install/upgrade layer can select repositories without embedding URLs in commands.


## Configured package operations — milestone 0.19

After configuring and updating a repository, the normal package commands can use repository names implicitly:

```sh
pux repo add stable https://repo.example.invalid/venpux 200 1 1
pux update
pux install hello
pux upgrade hello
```

`pux install <name>` and `pux upgrade <name>` inspect enabled repository caches in priority order. The local index must already exist, so metadata refresh remains an explicit `pux update` operation. Missing package archives are downloaded on demand from the selected repository, then verified against the cached index before any installation transaction begins. A repository-specific `require-signature=1` setting requires a trusted Ed25519 signature even when the global development setting is disabled.

## Nano 9.2 package definition

`packages/nano/` records the BLFS 13.1-systemd source, checksums, build commands, and expected installation layout for GNU nano 9.2. The final `.pux` binary is intentionally not committed yet; it must be built on the Venpux 13.1-systemd system.
