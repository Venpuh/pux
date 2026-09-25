# pux

`pux` is the package manager being developed for the Venpux Linux distribution.

The project is intentionally split into a distribution-independent core and a thin Venpux integration layer. Development and testing can therefore happen on ordinary Linux systems before integration into Venpux.

## Current status

Milestone 0.1.0-dev establishes:

- a small C17 CLI;
- a reproducible Makefile build;
- a command vocabulary for package management;
- initial package/repository specifications;
- a shell-based smoke-test suite.

The package and repository formats are specifications-in-progress and are expected to evolve before the first stable release.

## Build

Requirements: a C17-capable compiler and POSIX shell utilities.

```sh
make
make test
./build/pux version
./build/pux help
```

## Design goals

- native Linux implementation in C;
- deterministic and transactional package operations;
- cryptographically verifiable repositories and packages;
- safe filesystem handling;
- explicit dependency resolution;
- no dependency on Debian, RPM, pacman, or another package manager;
- a small core with Venpux-specific integration isolated behind clear interfaces.
