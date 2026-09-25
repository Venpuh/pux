# pux architecture

## Milestone 0.3

The project now contains an implemented package-manifest layer and a read-only `.pux` container validator. It still has no installation or extraction logic.

```text
CLI
 │
 ▼
Package manifest API
 │
 ├── parser
 ├── validator
 └── metadata printer

Future layers
 │
 ├── package container reader/validator
 ├── repository client
 ├── dependency resolver
 ├── package database
 ├── transaction engine
 └── filesystem / Venpux integration
```

## Core rules

1. The package manifest is parsed independently of the archive/container implementation.
2. Unknown manifest fields are rejected while format 1 is experimental.
3. Scalar metadata fields cannot be silently overridden by duplicates.
4. Installation is not implemented until archive safety and transaction semantics are specified.
5. Venpux-specific behavior remains isolated behind explicit interfaces.

## Container boundary

The container layer validates the ustar header checksum, archive member type, allowed path namespace, manifest uniqueness, archive terminator, and manifest contents before any future extraction transaction is allowed to begin.


## Build pipeline (Milestone 0.4)

`pux build` follows a deliberately deterministic pipeline:

```text
manifest + payload tree
        |
        v
manifest parse + validation
        |
        v
filesystem walk (regular files + directories only)
        |
        v
sort archive members by path
        |
        v
write ustar META/manifest + payload + end markers
```

Package creation currently rejects symbolic links and special files. Tar metadata uses UID/GID 0 and mtime 0 so repeated builds from identical inputs produce identical bytes. Long archive member names are rejected until a future format revision adds explicit ustar prefix support.

## Package database (Milestone 0.6)

The local package database stores one atomic record per installed package under:

```text
/var/lib/pux/packages/<name>.record
```

`PUX_DB_ROOT` may override the root for tests and development. A record contains a database-record header, the canonical package manifest, and a file ownership section. Each file entry is typed as `f` (regular file) or `d` (directory).

Database updates are written to a unique temporary record, flushed and synced, then atomically renamed into place. Database paths are validated as relative package-owned paths. The database layer does not install or remove filesystem content yet; that remains the responsibility of the future transaction engine.


## Dependency resolver (Milestone 0.7)

The resolver is deliberately separate from repository transport and transactions:

```text
repository manifests
        |
        v
   candidate set
        |
        v
 dependency resolver
        |
        +--> architecture filter
        +--> version constraints
        +--> provides lookup
        +--> conflict detection
        +--> dependency graph
        |
        v
 topological install plan
```

`pux resolve <package-name> <repository-dir>` is read-only. It does not install packages and does not touch the package database. The current repository representation is intentionally temporary; a signed repository index will replace the flat manifest scan in a later milestone.


## Install transaction (0.8)

`pux install <package.pux>` performs a local-package transaction. The package is fully validated and extracted into a staging directory on the target filesystem before destination conflicts and installed-file ownership are checked. Regular files are moved into place, then the package record is committed to the package database. If database registration fails, moved files and directories created by the transaction are rolled back.

The 0.8 transaction engine does not download packages and does not resolve a repository plan. Declared dependencies must already be satisfied by installed packages. `PUX_ROOT` and `PUX_DB_ROOT` provide isolated roots for tests and development.


## Milestone 0.9 repository-aware install

The development repository layer is intentionally simple: a flat directory of package archives. The resolver loads `.pux` files directly and can also load manifest files for development-only resolution tests.

The repository-aware CLI path is:

```text
pux install NAME REPOSITORY
        │
        ├── resolve dependency graph
        ├── obtain ordered .pux paths
        ├── validate all planned archives
        ├── preflight already-installed versions
        └── execute per-package install transactions
```

Each package install is atomic with respect to its own filesystem/database operation. The complete multi-package transaction is not yet globally atomic; cross-package rollback is a future milestone.


### Removal transaction

Removal uses the package database as the ownership source. Reverse dependencies are checked before changing the filesystem. Owned regular files are moved to a same-filesystem staging directory, the package record is removed, and the staged files are deleted. On database failure the file moves are reversed. Empty package-owned directories are cleaned after commit; shared or non-empty directories are preserved.
