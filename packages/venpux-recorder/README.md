# Venpux Recorder 0.2 package definition

This package definition records the first functional release of the native
Venpux Wayland screen recorder.

The source is pinned to commit
`a58f99a9dbac19ce54b0df43eec438e943435e1b` in the public
`Venpuh/venpux-recorder` repository.

Runtime architecture:

```
xdg-desktop-portal
        ↓
     PipeWire
        ↓
FFmpeg / libx264
        ↓
    H.264 / MP4
```

The GUI is intentionally small: record, stop, output path, timer, and backend
log. Pause/resume is not part of this 0.2 package.

The final `.pux` binary package is not committed here; it must be built on
Venpux so that the package contains files produced for the target system.
