# GNU nano 9.2 — BLFS 13.1-systemd

Source: the BLFS 13.1-systemd Nano-9.2 page.

- Version: 9.2
- Source: https://www.nano-editor.org/dist/v9/nano-9.2.tar.xz
- BLFS MD5: `9bb0f37945afa964d16bffd9e4da3106`
- Supplemental SHA-256: `05ecb99247b782e8a5b3a25ed4101dd034b0236902f7449bc9795b717642f7e9`

BLFS 13.1-systemd gives the following build commands:

```sh
./configure --prefix=/usr \
            --sysconfdir=/etc \
            --enable-utf8 \
            --docdir=/usr/share/doc/nano-9.2 &&
make

make install &&
install -v -m644 doc/{nano.html,sample.nanorc} /usr/share/doc/nano-9.2
```

BLFS states that Nano has no test suite. The installed programs are `nano` and `rnano` (symlink); installed directories include `/usr/share/nano` and `/usr/share/doc/nano-9.2`.

The package must be built on the target Venpux system, then staged into `pux`'s package format. The `rnano` symlink must remain a symbolic link.
