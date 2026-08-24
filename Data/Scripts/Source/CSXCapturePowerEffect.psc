Scriptname CSXCapturePowerEffect extends ActiveMagicEffect

Int Property Action Auto

Event OnEffectStart(Actor akTarget, Actor akCaster)
    If Action == 0
        CSXCaptureNative.TakeScreenshot()
    ElseIf Action == 1
        CSXCaptureNative.ToggleFrameSequence()
    ElseIf Action == 2
        CSXCaptureNative.ComposeLatestVideo()
    EndIf
EndEvent
