# Version 1 video composer

Video composition is a post-process owned entirely by this companion. It starts
only after CSX reports `kComplete` and reads the final `sequence.json`; the game
render thread never waits for a codec.

## Backend

Version 1 uses Windows components already present on normal Skyrim-capable
systems:

- Windows Imaging Component decodes CSX lossless BMP or PNG frames to 32-bit BGRA.
- Windows Media Foundation converts those frames and encodes H.264.
- Media Foundation's sink writer produces the MP4 container.

The companion does not redistribute a codec library, require an FFmpeg process,
or make CSX aware of an encoding format. If the Windows H.264 transform is not
available, the worker reports failure and leaves all lossless inputs untouched.

## Manifest mapping

- `timestampUs` is normalized to zero and converted to 100-nanosecond Media
  Foundation sample times.
- Timestamps must be unsigned, strictly increasing, and representable by Media
  Foundation. Invalid timelines fail before an output is created.
- The encoder maps source timestamps to a bounded constant-rate cadence and
  duplicates the preceding image across missing cadence slots. A dropped frame
  therefore remains a visible hold without changing playback speed.
- A Left or Right session produces one `-left.mp4` or `-right.mp4` file. If that
  name already exists, a numeric suffix preserves the earlier output.
- A Both session produces paired `-left.mp4` and `-right.mp4` files.
- The first written frame establishes dimensions; a later size change fails the
  composition instead of creating a malformed stream.
- `audio` remains false and no audio stream is created.

## Worker and ownership

`VideoComposer` owns one worker thread and serializes jobs. It decodes and
encodes away from Papyrus, the SKSE message callback, and the render thread. A
second request while queued or encoding is rejected as busy.

Version 1 owns and joins an active worker during process shutdown so no encoder
or notification callback can outlive the plugin. Windows codec and filesystem
calls are synchronous and are not cooperatively cancellable, so bounded shutdown
is not promised while composition is active. Finish or stop composition before
quitting Skyrim when immediate process exit matters.

Before encoding, the worker validates every source, output, temporary path, and
output suffix as one plan. Outputs are unique leaf names beside the frame-set
directory and cannot alias a source. Each job uses a new temporary name; an
unrelated pre-existing temporary file is never removed. A temporary output is
renamed with write-through semantics only after `IMFSinkWriter::Finalize`
succeeds. The worker never edits `sequence.json`, never deletes source frame
files, and never writes into the Skyrim game directory.
