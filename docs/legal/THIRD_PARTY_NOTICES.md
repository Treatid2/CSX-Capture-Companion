# Third-party notices

CSX Capture Companion is distributed under GPL-3.0-only. Its Windows DLL is
built against CommonLibSSE-NG 6.3.3 and the dependency set described below.
The corresponding licence texts are reproduced in the adjacent `licenses`
directory. Those third-party components remain subject to their own notices
and additional permissions.

## Incorporated or linked components

### CommonLibSSE-NG 6.3.3

Source: <https://github.com/alandtse/CommonLibSSE-NG/tree/v6.3.3>

Licence: GPL-3.0-or-later with the CommonLibSSE-NG Modding Exception and
GPL-3.0 Linking Exception (with Corresponding Source). The binary contains
CommonLibSSE-NG as a statically linked library. See:

- `licenses/CommonLibSSE-NG-GPL-3.0.txt`
- `licenses/CommonLibSSE-NG-EXCEPTIONS.md`
- `licenses/CommonLibSSE-NG-MIT-HERITAGE.txt`

The CommonLibSSE-NG build incorporates or exposes the following permissively
licensed components. Their notices are included even where a binary-form
exception or link-time dead-code elimination might otherwise make a notice
optional:

- `{fmt}` — Victor Zverovich and contributors — MIT
- `spdlog` — Gabi Melman and contributors — MIT
- DirectXMath — Microsoft Corporation — MIT
- DirectX Tool Kit — Microsoft Corporation — MIT
- DirectX Headers — Microsoft Corporation — MIT
- nlohmann/json — Niels Lohmann — MIT
- rapidcsv — BSD 3-Clause
- SimpleIni — Brodie Thiesfield — MIT
- toml11 — Toru Niina — MIT
- Xbyak — MITSUNARI Shigeo — BSD 3-Clause
- OpenVR — Valve Corporation — BSD 3-Clause

## Adapted source

The SkyUI MCM quest and `SKI_PlayerLoadGameAlias` construction in the
deterministic plugin builder was adapted from the proven pattern in Throwing
Stuff VR by Dominic Santangelo/Domek97. Throwing Stuff VR is licensed under
the MIT License. See `licenses/Throwing-Stuff-VR-MIT.txt`.

Source: <https://github.com/Domek97/Throwing-Stuff-VR>

## External runtime prerequisites

The ABI declarations in `include/CSXServiceAPI.h` and
`include/CSXScreenshotAPI.h` are maintained copies of the corresponding CSX
public headers. Community Shaders Expanded is GPL-3.0; those declarations and
this companion are covered by the GPLv3 text included as `LICENSE`.

Source: <https://github.com/ParticleTroned/skyrim-community-shaders>

The following projects or platform components are required or used at runtime
but their binaries are not redistributed in this archive:

- Community Shaders Expanded (CSX) 3.19-VR or later supplies the capture API
  and writes the lossless screenshots and frame sequences.
- SKSEVR supplies the runtime script-extender interface.
- SkyUI VR supplies SkyUI and the Mod Configuration Menu framework.
- Windows Imaging Component decodes BMP and PNG source frames.
- Windows Media Foundation supplies the H.264 encoder and MP4 sink writer.

Windows Imaging Component and Windows Media Foundation are operating-system
services. CSX Capture Companion does not redistribute Microsoft codecs or an
FFmpeg binary.

## Build-time dependency

Mutagen.Bethesda.Skyrim 0.54.2 by Noggog and the Mutagen contributors is used
only by the repository's deterministic ESP build tool. Mutagen is
GPL-3.0-only. No Mutagen binaries are included in the mod archive.

Source: <https://github.com/Mutagen-Modding/Mutagen>
