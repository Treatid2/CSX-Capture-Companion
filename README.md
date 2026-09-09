# CSX Capture Companion

An intentionally small Skyrim SKSE mod demonstrating the lossless capture API
provided by Community Shaders Expanded (CSX).

The companion owns player-facing powers, SkyUI MCM configuration, and
post-process video composition. CSX owns only screenshots and lossless frame
sets. Audio is outside the first version.

## First-version controls

- **Capture Screenshot** lesser power: requests one CSX screenshot.
- **Toggle Frame Capture** lesser power: starts or stops a lossless CSX frame set.
- **Compose Latest Capture** lesser power: queues the latest completed frame set
  for asynchronous MP4 composition.
- MCM toggles install or remove those powers and select Left, Right, or Both eyes.

The 0.1.1 demonstrator requests PNG stills and a bounded 300-frame BMP
sequence sampled every 12 rendered game frames. It uses one eye output or two
synchronized Left/Right outputs, disables clipboard and CSX preview packaging,
records backpressure in the manifest, and never starts a second sequence while
the first receipt is active.

The native plugin discovers CSX's `CSXR` service registry through SKSE
messaging, queries `csx.screenshot` major version 1, and sends the same
asynchronous JSON commands used by CSX and DevBench. The powers are therefore
examples of public API control surfaces rather than alternate capture
implementations. The former capture-specific `CSAP` vtable is not used.

## Output

CSX writes screenshots beneath the Windows Pictures Known Folder and frame sets
beneath the Windows Videos Known Folder. This mod does not redirect either path
to the Skyrim installation. After CSX reports a sequence complete, this mod can
obtain the final manifest path from the terminal Screenshot API receipt.

## Video composition

Video encoding belongs in this repository, never in CSX. A companion-owned
worker consumes `sequence.json` only after CSX reports the session complete.
Windows Imaging Component decodes the lossless BMP or PNG frames and Windows Media Foundation
encodes H.264 in an MP4 container. Encoding never runs on the render thread,
source frames are retained, and output is committed only after finalization.

Left and Right sessions produce one `-left.mp4` or `-right.mp4` beside the frame
set. Both-eye sessions produce one double-width `-sbs.mp4`, with the left eye in
the left half and the right eye in the right half. Existing outputs are
preserved with a numbered filename. Manifest timestamps drive sample timing, so
dropped frames repeat the preceding image rather than silently changing
playback speed; malformed, excessive, or non-monotonic timelines are rejected
before any output is created. Only bounded manifests and explicitly committed
artifacts are consumed. Version 1 has no audio.

## Build status

The repository contains the native demonstrator, deterministic ESL-flagged
plugin builder, compiled and reviewable Papyrus assets, and a package script.
The MCM quest follows the proven `ThrowingStuffVR` SkyUI structure, including
the `SKI_PlayerLoadGameAlias` reload bridge.

Configure with a CommonLibSSE-NG package available to CMake, or point at a
CommonLibSSE-NG source checkout, then build normally:

```powershell
cmake -S . -B build -DCOMMONLIBSSE_SOURCE_DIR=<path-to-CommonLibSSE-NG>
cmake --build build --config Release
```

For a complete Codex package build, including Papyrus, records, native DLL,
install staging, and ZIP creation:

```powershell
& .\tools\Build-Package.ps1
```

The resulting archive is `dist\CSXCaptureCompanion-0.1.1.zip`. The deterministic
record builder can be run or verified independently:

```powershell
dotnet run --project tools\BuildPlugin\BuildPlugin.csproj -c Release -- Data\CSXCaptureCompanion.esp
dotnet run --project tools\BuildPlugin\BuildPlugin.csproj -c Release -- --verify Data\CSXCaptureCompanion.esp
```

Runtime prerequisites are SKSE, Address Library as required by the selected
CommonLibSSE-NG target, CSX 3.19-VR or later, and SkyUI for the MCM.
Video composition additionally requires the Windows Media Foundation H.264
encoder supplied with standard Windows 10 and Windows 11 installations.

The video backend and its dependency boundary are recorded in
`docs/EncoderDesign.md`.

## License

Copyright (C) 2026 Treatid2.

CSX Capture Companion is free software licensed under the GNU General Public
License, version 3 (`GPL-3.0-only`). See [LICENSE](LICENSE) for the complete
license text. Corresponding source code is published at
<https://github.com/Treatid2/CSX-Capture-Companion>.

Third-party copyright, licence, exception, and source notices are collected in
[docs/legal/THIRD_PARTY_NOTICES.md](docs/legal/THIRD_PARTY_NOTICES.md). The
release archive includes that notice and every applicable licence text under
`Docs/CSX Capture Companion/Legal`.
