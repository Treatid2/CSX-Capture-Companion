# Plugin record contract

The distributable ESP/ESL should contain only the forms needed to expose the API
demonstrator in Skyrim's Powers menu:

| Editor ID | Record | Purpose |
| --- | --- | --- |
| `CSXCaptureQuest` | QUST | Starts the MCM script and owns its spell properties. |
| `CSXCaptureScreenshotEffect` | MGEF | Scripted, zero-cost self effect; `Action = 0`. |
| `CSXCaptureToggleEffect` | MGEF | Scripted, zero-cost self effect; `Action = 1`. |
| `CSXCaptureComposeEffect` | MGEF | Scripted, zero-cost self effect; `Action = 2`. |
| `CSXCaptureScreenshotPower` | SPEL | Lesser Power invoking the screenshot effect. |
| `CSXCaptureTogglePower` | SPEL | Lesser Power invoking the start/stop effect. |
| `CSXCaptureComposePower` | SPEL | Lesser Power queuing completed frames for composition. |

Attach `CSXCapturePowerEffect` to all three magic effects and set its `Action`
property as shown. Attach `CSXCaptureMCM` to the quest, fill all three spell
properties, and start the quest at game load. The quest contains a forced-player
alias with `SKI_PlayerLoadGameAlias`, matching the working `ThrowingStuffVR`
MCM reload pattern.

`tools/BuildPlugin` creates these seven records at stable local FormIDs
`0x800`–`0x806`, sets the ESL flag, and reopens the output with Mutagen to verify
the record count, identities, MCM properties, alias bridge, and action values.

The powers contain no capture logic. They are deliberately thin control
surfaces over `CSXCaptureNative`, which in turn calls the CSX API.

The **Compose Video** power consumes only a completed `sequence.json`; it never
asks CSX to encode video or capture audio.
