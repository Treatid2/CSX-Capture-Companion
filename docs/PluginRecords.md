# Plugin record contract

The distributable ESP/ESL should contain only the forms needed to expose the API
demonstrator in Skyrim's Powers menu:

| Editor ID | Record | Purpose |
| --- | --- | --- |
| `CSXCaptureQuest` | QUST | Starts the MCM script and owns its spell properties. |
| `CSXCaptureScreenshotEffect` | MGEF | Scripted, zero-cost self effect; `Action = 0`. |
| `CSXCaptureToggleEffect` | MGEF | Scripted, zero-cost self effect; `Action = 1`. |
| `CSXCaptureScreenshotPower` | SPEL | Lesser Power invoking the screenshot effect. |
| `CSXCaptureTogglePower` | SPEL | Lesser Power invoking the start/stop effect. |

Attach `CSXCapturePowerEffect` to both magic effects and set its `Action`
property as shown. Attach `CSXCaptureMCM` to the quest, fill both spell
properties, and start the quest at game load. Mark the plugin ESL-capable if the
chosen authoring tool confirms all records and references are compacted safely.

The powers contain no capture logic. They are deliberately thin control
surfaces over `CSXCaptureNative`, which in turn calls the CSX API.

An optional **Compose Video** lesser power should be added only when the
companion encoder worker is implemented. It must consume a completed
`sequence.json`; it must not ask CSX to encode or capture audio.
