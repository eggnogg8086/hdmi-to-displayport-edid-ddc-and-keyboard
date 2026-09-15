ESP32 Brightness Bridge v10.8.1.1 - Predictive DIM-to-OFF
=====================================================

DEVELOPER / DISTRIBUTION WORKFLOW

1. Run build.bat on your development PC.
2. Run MakeRelease.bat.
3. Give release\ESP32BrightnessBridge_v10_8_1.zip to the user.

The recipient only needs to extract the ZIP and double-click Install.cmd.
No Visual Studio, compiler, SDK, or source code is required.

USER SETTINGS
-------------
The installed agent reads:

    C:\ProgramData\ESP32BrightnessBridge\settings.ini

The file is hot-reloaded about once per second; saving it in Notepad does not
require restarting the agent. A Start Menu shortcut named "Edit Settings" opens
it directly. Existing settings.ini files are preserved across upgrades.

Default settings:

    [Display]
    DimPercent=50

    [Prediction]
    Enabled=1
    LearningEnabled=1
    RequireLearnedSample=1
    CheckDisplayRequired=1
    AdvanceMs=1400
    PreOffConfirmTimeoutMs=2500
    FallbackDimToOffMs=5000
    MinimumPredictionDelayMs=500
    MinimumLearnIntervalMs=1500
    MaximumLearnIntervalMs=15000

HOW PREDICTION WORKS
--------------------
- Prediction NEVER starts from raw keyboard/mouse idle time.
- Windows GUID_SESSION_DISPLAY_STATUS remains authoritative.
- Windows ON: normal user brightness.
- Windows DIM: apply DimPercent, start one DIM->OFF timing cycle.
- The first cycle is calibration-only by default (RequireLearnedSample=1).
- The agent learns the real DIM->OFF interval from completed Windows cycles.
- AdvanceMs controls how long before the learned/expected OFF moment VCP 0x10=0
  is sent. With AdvanceMs=1400, pre-OFF is targeted about 1.4 seconds before the
  expected Windows OFF event.
- Just before predicted pre-OFF, the agent checks ES_DISPLAY_REQUIRED. If an app
  is requesting the display stay on, prediction is cancelled and Windows remains
  in control.
- If Windows does not confirm OFF within PreOffConfirmTimeoutMs after a predicted
  pre-OFF, the bridge restores a visible DIM level automatically.
- If timing cannot be verified or a predictive DDC write fails, the agent simply
  waits for Windows' real OFF event. This is the fail-open path.
- The actual Windows OFF event is also used to learn timing and remains the final
  authoritative fallback.

MANUAL TIMING
-------------
To disable learning and use a fixed DIM->OFF interval:

    LearningEnabled=0
    FallbackDimToOffMs=5000
    AdvanceMs=1400

This means: after Windows reports DIM, expect Windows OFF at ~5000 ms and send
backlight OFF at ~3600 ms after DIM (5000 - 1400).

To disable anticipation completely:

    Enabled=0

Windows DIM will still use DimPercent, but VCP 0x10=0 is not sent until Windows
itself reports OFF.

SAFETY MODEL
------------
- No PowerCreateRequest / PowerSetRequest / SetThreadExecutionState calls.
- The app does not ask Windows to keep the screen or system awake.
- Sleep is never delayed by a fade wait.
- Shutdown, restart, and sign-out freeze brightness and never set it to zero.
- Lid-close and actual Windows OFF remain independent best-effort OFF paths.
- A real user/DDC brightness change during DIM cancels anticipation rather than
  being overwritten.
- Sleep, lid, shutdown, resume, Windows ON, and other interrupted cycles are not
  accepted as DIM->OFF learning samples.
- The emergency hotkey remains Ctrl+Alt+Shift+B.

Start Menu tools after installation:
- Edit Settings
- Emergency Recover
- Status and Diagnostics
- View Log
- Uninstall ESP32 Brightness Bridge
