# pux

`pux` is the package manager being developed for the Venpux Linux distribution.

The project is intentionally split into a distribution-independent core and a thin Venpux integration layer. Development and testing can therefore happen on ordinary Linux systems before integration into Venpux.

## Current status

Milestone 0.6.0-dev adds a persistent local installed-package database on top of safe `.pux` extraction and deterministic package creation.

Implemented:

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
- `list` and installed-package `info` queries;
- automated CLI, manifest, container, package-build, extraction, and database tests.

Not implemented yet:

- dependency resolver;
- transactions;
- repository client;
- installation/removal.

## Build

Requirements: a C17-capable compiler and POSIX shell utilities.

```sh
make
make test
./build/pux version
./build/pux package validate samples/hello.pux.manifest
./build/pux package info samples/hello.pux.manifest
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
