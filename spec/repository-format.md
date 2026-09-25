# Repository format — milestone 0.9

For development and local testing, a repository is a directory containing installable `.pux` package archives. The resolver also accepts `.manifest` and `.pux.manifest` development files so dependency resolution can be tested without building archives.

Current development layout:

```text
repo/
├── glibc-2.44-1-x86_64.pux
├── libidn2-2.3.7-1-x86_64.pux
└── hello-1.0.0-1-x86_64.pux
```

`pux install <package-name> <repository-dir>` loads package metadata from the directory, resolves dependencies, validates the selected package archives, and executes the resulting plan dependency-first.

The final remote repository format is still open. It is expected to add a signed index, repository identity, package hashes, freshness rules, key rotation, mirrors, and transport URLs. The client will eventually verify repository metadata and package content before installation.
