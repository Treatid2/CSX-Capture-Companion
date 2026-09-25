Scriptname CSXCaptureMCM extends SKI_ConfigBase

Spell Property ScreenshotPower Auto
Spell Property ToggleCapturePower Auto
Spell Property ComposeVideoPower Auto

Int ScreenshotPowerOption
Int ToggleCapturePowerOption
Int ComposeVideoPowerOption
Int ComposeNowOption

Bool ScreenshotPowerInstalled = True
Bool ToggleCapturePowerInstalled = True
Bool ComposeVideoPowerInstalled = True

Int Function GetVersion()
    Return 1
EndFunction

Event OnConfigInit()
    ModName = "CSX Capture Companion"
    Pages = New String[1]
    Pages[0] = "Controls"
    ApplyPowerState()
EndEvent

Event OnVersionUpdate(Int newVersion)
    If newVersion >= 1
        ModName = "CSX Capture Companion"
        Pages = New String[1]
        Pages[0] = "Controls"
        ApplyPowerState()
    EndIf
EndEvent

Event OnPageReset(String page)
    If page == ""
        LoadCustomContent("skyui/mcm_splash.dds", 0.0, 0.0)
        Return
    EndIf

    UnloadCustomContent()
    SetCursorFillMode(TOP_TO_BOTTOM)

    Bool apiAvailable = CSXCaptureNative.IsAvailable()
    Int unavailableFlags = OPTION_FLAG_NONE
    If !apiAvailable
        unavailableFlags = OPTION_FLAG_DISABLED
    EndIf

    AddHeaderOption("Capture API demonstrator")
    ScreenshotPowerOption = AddToggleOption("Install Screenshot power", ScreenshotPowerInstalled)
    ToggleCapturePowerOption = AddToggleOption("Install Start/Stop Capture power", ToggleCapturePowerInstalled)
    ComposeVideoPowerOption = AddToggleOption("Install Compose Video power", ComposeVideoPowerInstalled)

    AddHeaderOption("Status and post-process")
    AddTextOption("CSX capture API", ApiStatusName(), OPTION_FLAG_DISABLED)
    AddTextOption("Capture state", CaptureStateName(), OPTION_FLAG_DISABLED)
    ComposeNowOption = AddTextOption("Compose latest completed capture", "Run", unavailableFlags)
    AddTextOption("Composer", CSXCaptureNative.GetComposeStatus(), OPTION_FLAG_DISABLED)
EndEvent

Event OnOptionSelect(Int option)
    If option == ScreenshotPowerOption
        ScreenshotPowerInstalled = !ScreenshotPowerInstalled
        SetToggleOptionValue(ScreenshotPowerOption, ScreenshotPowerInstalled)
        ApplyPowerState()
    ElseIf option == ToggleCapturePowerOption
        ToggleCapturePowerInstalled = !ToggleCapturePowerInstalled
        SetToggleOptionValue(ToggleCapturePowerOption, ToggleCapturePowerInstalled)
        ApplyPowerState()
    ElseIf option == ComposeVideoPowerOption
        ComposeVideoPowerInstalled = !ComposeVideoPowerInstalled
        SetToggleOptionValue(ComposeVideoPowerOption, ComposeVideoPowerInstalled)
        ApplyPowerState()
    ElseIf option == ComposeNowOption
        If CSXCaptureNative.ComposeLatestVideo()
            SetTextOptionValue(ComposeNowOption, "Queued")
        Else
            SetTextOptionValue(ComposeNowOption, "Not ready")
        EndIf
    EndIf
EndEvent

Event OnOptionDefault(Int option)
    If option == ScreenshotPowerOption
        ScreenshotPowerInstalled = True
        SetToggleOptionValue(option, True)
        ApplyPowerState()
    ElseIf option == ToggleCapturePowerOption
        ToggleCapturePowerInstalled = True
        SetToggleOptionValue(option, True)
        ApplyPowerState()
    ElseIf option == ComposeVideoPowerOption
        ComposeVideoPowerInstalled = True
        SetToggleOptionValue(option, True)
        ApplyPowerState()
    EndIf
EndEvent

Event OnOptionHighlight(Int option)
    If option == ScreenshotPowerOption
        SetInfoText("Adds a lesser power that requests one lossless screenshot using CSX's Screenshot settings.")
    ElseIf option == ToggleCapturePowerOption
        SetInfoText("Adds a lesser power that starts or stops a lossless sequence using CSX's Frame Capture settings.")
    ElseIf option == ComposeVideoPowerOption
        SetInfoText("Adds a lesser power that queues the latest completed sequence for companion-owned MP4 composition.")
    ElseIf option == ComposeNowOption
        SetInfoText("Queues the latest completed sequence.json. CSX does not encode video and the lossless source frames are retained.")
    EndIf
EndEvent

String Function ApiStatusName()
    If CSXCaptureNative.IsAvailable()
        Return "Available"
    EndIf
    Return "Unavailable"
EndFunction

String Function CaptureStateName()
    Int captureState = CSXCaptureNative.GetCaptureState()
    If captureState == 0
        Return "Idle"
    ElseIf captureState == 1
        Return "Capturing"
    ElseIf captureState == 2
        Return "Flushing"
    ElseIf captureState == 3
        Return "Complete"
    ElseIf captureState == 4
        Return "Failed"
    EndIf
    Return "Unavailable"
EndFunction

Function ApplyPowerState()
    Actor player = Game.GetPlayer()
    If !player
        Return
    EndIf

    If ScreenshotPower
        If ScreenshotPowerInstalled
            player.AddSpell(ScreenshotPower, False)
        Else
            player.RemoveSpell(ScreenshotPower)
        EndIf
    EndIf
    If ToggleCapturePower
        If ToggleCapturePowerInstalled
            player.AddSpell(ToggleCapturePower, False)
        Else
            player.RemoveSpell(ToggleCapturePower)
        EndIf
    EndIf
    If ComposeVideoPower
        If ComposeVideoPowerInstalled
            player.AddSpell(ComposeVideoPower, False)
        Else
            player.RemoveSpell(ComposeVideoPower)
        EndIf
    EndIf
EndFunction
