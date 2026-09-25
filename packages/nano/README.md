# Nano 9.2 package definition

This is the first real Venpux package definition in the `pux` repository.

The build instructions are derived from BLFS 13.1-systemd. The final `.pux`
package must be built on Venpux, not on the development laptop, so that the
result is produced by the Venpux toolchain and matches its actual filesystem.

`pux` now supports the symbolic link required by BLFS (`rnano -> nano`).
