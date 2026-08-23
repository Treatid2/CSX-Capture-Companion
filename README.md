# CSX Capture Companion

An intentionally small Skyrim SKSE mod demonstrating the lossless capture API
provided by Community Shaders Expanded (CSX).

The companion owns player-facing powers, SkyUI MCM configuration, and eventual
post-process video composition. CSX owns only screenshots and lossless frame
sets. Audio is outside the first version.

## First-version controls

- **Capture Screenshot** lesser power: requests one CSX screenshot.
- **Toggle Frame Capture** lesser power: starts or stops a lossless CSX frame set.
- MCM toggles install or remove those powers and select Left, Right, or Both eyes.

The native plugin discovers CSX revision 5 through SKSE messaging and calls only
`ICSCaptureInterface001`. The powers are therefore examples of API control
surfaces rather than alternate capture implementations.

## Output

CSX writes screenshots beneath the Windows Pictures Known Folder and frame sets
beneath the Windows Videos Known Folder. This mod does not redirect either path
to the Skyrim installation. After CSX reports a sequence complete, this mod can
obtain its UTF-8 directory through `CopySequencePath`.

## Video composition boundary

Video encoding belongs in this repository, never in CSX. The first source slice
establishes the capture and MCM boundary; the encoder worker and optional
**Compose Video** power will follow here. It will consume `sequence.json` only
after CSX has finished the session. No live encoding and no audio are planned
for version 1.

## Build status

This repository currently contains the native demonstrator, Papyrus sources,
and the record contract. A distributable build additionally needs the plugin
records described in `docs/PluginRecords.md` and compiled Papyrus scripts.

Configure with a CommonLibSSE-NG package available to CMake, or point at a
CommonLibSSE-NG source checkout, then build normally:

```powershell
cmake -S . -B build -DCOMMONLIBSSE_SOURCE_DIR=<path-to-CommonLibSSE-NG>
cmake --build build --config Release
```

Runtime prerequisites are SKSE, Address Library as required by the selected
CommonLibSSE-NG target, CSX build 12 or later, and SkyUI for the MCM.

The proposed video backend and its dependency boundary are recorded in
`docs/EncoderDesign.md`.
