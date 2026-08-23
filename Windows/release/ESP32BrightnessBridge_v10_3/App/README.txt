ESP32 Brightness Bridge v10.3 - Developer Package
==================================================

DEVELOPER / DISTRIBUTION WORKFLOW

1. Run build.bat on your development PC.
2. Run MakeRelease.bat.
3. Give the generated ZIP in the release folder to the user.

The generated end-user ZIP is intentionally simple:

    ESP32BrightnessBridge_v10_3\
        Install.cmd        <- user double-clicks this to install/update
        Uninstall.cmd      <- optional direct uninstall
        App\               <- internal support files; user can ignore this

The user does NOT need Visual Studio, MSVC, the Windows SDK, source code,
or any build tools.

After installation, normal maintenance is available through the Start Menu:
- Emergency Recover
- Status and Diagnostics
- View Log
- Uninstall ESP32 Brightness Bridge

The program can also be uninstalled from:
Settings > Apps > Installed apps > ESP32 Brightness Bridge

Emergency recovery hotkey while the agent is running:
Ctrl+Alt+Shift+B
