# pux

`pux` is the package manager being developed for the Venpux Linux distribution.

The project is intentionally split into a distribution-independent core and a thin Venpux integration layer. Development and testing can therefore happen on ordinary Linux systems before integration into Venpux.

## Current status

Milestone 0.2.0-dev implements a real package-manifest parser and validator in C17.

Implemented:

- C17 CLI;
- reproducible Makefile build;
- versioned manifest format 1;
- parsing of scalar and repeated metadata fields;
- duplicate/unknown-field rejection;
- manifest validation;
- package `validate` and `info` commands;
- automated CLI and package-manifest tests.

Not implemented yet:

- `.pux` archive reader/writer;
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
