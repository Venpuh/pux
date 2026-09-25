# pux architecture

## Milestone 0.2

The project now contains an implemented package-manifest layer. It deliberately has no filesystem installation logic yet.

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
 ├── package container reader/writer
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
