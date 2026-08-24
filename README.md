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

The native plugin discovers CSX revision 5 through SKSE messaging and calls only
`ICSCaptureInterface001`. The powers are therefore examples of API control
surfaces rather than alternate capture implementations.

## Output

CSX writes screenshots beneath the Windows Pictures Known Folder and frame sets
beneath the Windows Videos Known Folder. This mod does not redirect either path
to the Skyrim installation. After CSX reports a sequence complete, this mod can
obtain its UTF-8 directory through `CopySequencePath`.

## Video composition

Video encoding belongs in this repository, never in CSX. A companion-owned
worker consumes `sequence.json` only after CSX reports the session complete.
Windows Imaging Component decodes the PNG frames and Windows Media Foundation
encodes H.264 in an MP4 container. Encoding never runs on the render thread,
source frames are retained, and output is committed only after finalization.

Left and Right sessions produce one `-left.mp4` or `-right.mp4` beside the frame
set. Both-eye sessions produce a synchronized pair. Manifest timestamps drive
sample timing, so dropped frames extend the preceding sample rather than
silently changing playback speed. Version 1 has no audio.

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

The resulting archive is `dist\CSXCaptureCompanion-0.1.0.zip`. The deterministic
record builder can be run or verified independently:

```powershell
dotnet run --project tools\BuildPlugin\BuildPlugin.csproj -c Release -- Data\CSXCaptureCompanion.esp
dotnet run --project tools\BuildPlugin\BuildPlugin.csproj -c Release -- --verify Data\CSXCaptureCompanion.esp
```

Runtime prerequisites are SKSE, Address Library as required by the selected
CommonLibSSE-NG target, CSX build 12 or later, and SkyUI for the MCM.
Video composition additionally requires the Windows Media Foundation H.264
encoder supplied with standard Windows 10 and Windows 11 installations.

The video backend and its dependency boundary are recorded in
`docs/EncoderDesign.md`.
