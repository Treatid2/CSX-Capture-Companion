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
- Current manifests admit only CSX's terminal per-frame states. Completed
  states provide images; explicit failed, cancelled, stopped, rejected, or
  dropped states provide hold positions. Unknown states fail before planning.
- The encoder starts with the rounded median scheduled cadence and raises it,
  up to 120 fps, until every recorded slot has a distinct sample position.
  Timelines that cannot fit the 60,000-sample bound are rejected.
- Missing interior and trailing slots duplicate the preceding image. The final
  scheduled slot receives one complete sample interval, so recorded trailing
  drops remain visible without shortening playback.
- A Left or Right session produces one `-left.mp4` or `-right.mp4` file.
- A Both session produces one half-SBS-compatible `-sbs.mp4`. The left eye
  occupies the left half and the right eye occupies the right half.
- Source eyes are scaled proportionally when required to keep the encoded canvas
  within 3840x2160. The lossless input files are not modified.
- The encoder selects quality-based variable bitrate at maximum quality and
  maximum quality-over-speed. Large files are an intentional tradeoff for
  retaining detail in high-resolution VR captures.
- The first written frame establishes dimensions; a later source-size change
  fails the composition instead of creating a malformed stream.
- Playback timing follows the manifest. The composer holds missing scheduled
  slots but cannot synthesize motion between sparsely captured frames.
- `audio` remains false and no audio stream is created.

## Worker and ownership

`VideoComposer` owns one worker thread and serializes jobs. It decodes and
encodes away from Papyrus, the SKSE message callback, and the render thread. A
second request while queued or encoding is rejected as busy.
Once the deterministic output for a manifest exists, another request reports
that completed output without encoding a numbered duplicate.

Version 1 owns and joins an active worker during process shutdown so no encoder
or notification callback can outlive the plugin. Windows codec and filesystem
calls are synchronous and are not cooperatively cancellable, so bounded shutdown
is not promised while composition is active. Finish or stop composition before
quitting Skyrim when immediate process exit matters.

Before encoding, the worker validates every source, output, temporary path, and
output suffix as one plan. It accepts at most two output streams, bounds the
manifest and timeline sizes, and consumes only artifacts marked committed by
CSX. Outputs are unique leaf names beside the frame-set directory and cannot
alias a source. Each job uses a new temporary name; an unrelated pre-existing
temporary file is never removed. A temporary output is renamed with
write-through semantics only after `IMFSinkWriter::Finalize` succeeds. The
worker never edits `sequence.json`, never deletes source frame files, and never
writes into the Skyrim game directory.
