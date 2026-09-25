# pux

`pux` is the package manager being developed for the Venpux Linux distribution.

The project is intentionally split into a distribution-independent core and a thin Venpux integration layer. Development and testing can therefore happen on ordinary Linux systems before integration into Venpux.

## Current status

Milestone 0.4.0-dev adds deterministic `.pux` package creation on top of the package-manifest parser and ustar container validator.

Implemented:

- C17 CLI;
- reproducible Makefile build;
- versioned manifest format 1;
- parsing of scalar and repeated metadata fields;
- duplicate/unknown-field rejection;
- manifest validation;
- package `validate` and `info` commands;
- deterministic `build <manifest> <payload-dir> <output.pux>` command;
- automated CLI, manifest, container, and package-build tests.

Not implemented yet:

- dependency resolver;
- package database;
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

Milestone 0.4 reads and writes an uncompressed POSIX ustar `.pux` package containing `META/manifest` and `payload/`. The command `pux build <manifest> <payload-dir> <output.pux>` validates its manifest, collects only regular files and directories, sorts payload entries for deterministic output, and emits fixed metadata. `pux package validate <file>` validates the resulting archive. Extraction is intentionally not implemented yet.
