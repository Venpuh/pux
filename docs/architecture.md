# pux architecture

## Layers

```text
CLI
 │
 ▼
Command layer
 │
 ├── package operations
 ├── repository operations
 └── build operations
 │
 ▼
Core
 │
 ├── package parser
 ├── repository client
 ├── dependency resolver
 ├── package database
 ├── transaction engine
 ├── archive reader/writer
 ├── downloader
 ├── verifier
 └── filesystem layer
 │
 ▼
Platform integration
 │
 └── Venpux-specific policies/hooks
```

## Core rules

1. The core must not assume a Debian/RPM/pacman style package database.
2. Installation and removal must be transaction based.
3. Archive extraction must reject path traversal (`..`, absolute paths, unsafe links) and similar filesystem attacks.
4. Repository metadata and package payloads must be verified before installation.
5. Venpux-specific behavior should live behind explicit interfaces rather than being scattered through the core.
6. The command-line interface is part of the public UX and should remain stable once the first stable release exists.

## Initial command set

- `search`
- `info`
- `install`
- `remove`
- `update`
- `upgrade`
- `list`
- `verify`
- `build`
- `repo`

Additional commands may be added before 1.0.
