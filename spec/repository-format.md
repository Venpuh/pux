# Repository format — draft

This is an initial design draft.

A repository will contain package files plus signed metadata describing available versions.

Proposed layout:

```text
repo/
├── index
├── signatures/
└── packages/
    ├── h/hello-1.0.0-1-x86_64.pux
    └── ...
```

The final specification will define:

- repository identity;
- package records;
- architecture and version fields;
- dependency metadata;
- SHA-256 (or a successor) package hashes;
- timestamps and freshness rules;
- signing and key rotation;
- mirrors and transport URLs.

The client must verify repository metadata before trusting package records, then verify package content before installation.
