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
