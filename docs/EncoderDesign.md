# Version 1 video composer

Video composition is a post-process owned entirely by this companion. It starts
only after CSX reports `kComplete` and reads `sequence.json`; the game render
thread never waits for a codec.

## Proposed first backend

The smallest redistributable default is VP9 in a WebM container:

- Windows Imaging Component decodes CSX PNG frames.
- `libvpx` performs VP9 encoding.
- `libwebm` writes the WebM container.

Both WebM libraries use permissive BSD-style terms. This gives the demonstrator
one dependable, royalty-conscious output path without making CSX a codec host.
FFmpeg remains a useful optional backend, but its LGPL/GPL configuration and
redistribution obligations need to be treated as a deliberate packaging choice,
not hidden inside CSX.

## Manifest mapping

- `timestampUs` is the presentation clock. The composer preserves it rather
  than assuming the game maintained a fixed frame rate.
- A Left or Right session produces one `.webm` file.
- A Both session initially produces paired `-left.webm` and `-right.webm`
  files. Side-by-side composition can be a later explicit option.
- Dropped records extend the preceding frame to the next valid timestamp; the
  composer reports the dropped count in its result.
- The output is written beside the completed frame-set directory, already under
  the Windows Videos Known Folder unless the user configured an absolute path.

## Worker and ownership

The encoder runs on one companion-owned worker thread and supports cancellation
between frames. It never edits `sequence.json`, never deletes source frames, and
writes to a temporary output before an atomic rename. A future MCM action or
optional **Compose Video** power queues this worker and reports progress through
the companion UI. Version 1 has no audio stream.
