Scriptname CSXCaptureMCM extends SKI_ConfigBase

Spell Property ScreenshotPower Auto
Spell Property ToggleCapturePower Auto

Int ScreenshotPowerOption
Int ToggleCapturePowerOption
Int EyeOption
Bool ScreenshotPowerInstalled = True
Bool ToggleCapturePowerInstalled = True
Int CaptureEye = 0

Event OnConfigInit()
    ModName = "CSX Capture Companion"
    ApplyPowerState()
    CSXCaptureNative.SetEye(CaptureEye)
EndEvent

Event OnPageReset(String asPage)
    SetCursorFillMode(TOP_TO_BOTTOM)
    AddHeaderOption("Capture API demonstrator")
    ScreenshotPowerOption = AddToggleOption("Install Screenshot power", ScreenshotPowerInstalled)
    ToggleCapturePowerOption = AddToggleOption("Install Start/Stop Capture power", ToggleCapturePowerInstalled)
    String[] eyeNames = New String[3]
    eyeNames[0] = "Left"
    eyeNames[1] = "Right"
    eyeNames[2] = "Both"
    EyeOption = AddMenuOption("Capture eye", eyeNames[CaptureEye])
    If !CSXCaptureNative.IsAvailable()
        AddTextOption("CSX capture API", "Unavailable", OPTION_FLAG_DISABLED)
    EndIf
EndEvent

Event OnOptionSelect(Int aiOption)
    If aiOption == ScreenshotPowerOption
        ScreenshotPowerInstalled = !ScreenshotPowerInstalled
        SetToggleOptionValue(ScreenshotPowerOption, ScreenshotPowerInstalled)
        ApplyPowerState()
    ElseIf aiOption == ToggleCapturePowerOption
        ToggleCapturePowerInstalled = !ToggleCapturePowerInstalled
        SetToggleOptionValue(ToggleCapturePowerOption, ToggleCapturePowerInstalled)
        ApplyPowerState()
    EndIf
EndEvent

Event OnOptionMenuOpen(Int aiOption)
    If aiOption == EyeOption
        String[] eyeNames = New String[3]
        eyeNames[0] = "Left"
        eyeNames[1] = "Right"
        eyeNames[2] = "Both"
        SetMenuDialogOptions(eyeNames)
        SetMenuDialogStartIndex(CaptureEye)
        SetMenuDialogDefaultIndex(0)
    EndIf
EndEvent

Event OnOptionMenuAccept(Int aiOption, Int aiIndex)
    If aiOption == EyeOption
        CaptureEye = aiIndex
        String[] eyeNames = New String[3]
        eyeNames[0] = "Left"
        eyeNames[1] = "Right"
        eyeNames[2] = "Both"
        SetMenuOptionValue(EyeOption, eyeNames[CaptureEye])
        CSXCaptureNative.SetEye(CaptureEye)
    EndIf
EndEvent

Function ApplyPowerState()
    Actor player = Game.GetPlayer()
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
EndFunction
