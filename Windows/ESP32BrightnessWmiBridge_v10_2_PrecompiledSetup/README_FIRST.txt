ESP32 Brightness Bridge v10.2 - Precompiled Easy Setup
=====================================================

FOR THE END USER
----------------
1. Extract the supplied ZIP.
2. Double-click Install.cmd.
3. Accept the Windows administrator/UAC prompt.
4. The installer verifies DDC communication before enabling automatic control.

NO BUILD TOOLS ARE REQUIRED.
The installer never invokes Visual Studio, MSVC, build.bat, or any compiler.

TO UNINSTALL
------------
Use either:
  Settings > Apps > Installed apps > ESP32 Brightness Bridge > Uninstall
or double-click Uninstall.cmd from the original extracted folder.

EMERGENCY RECOVERY
------------------
If the panel is black but Windows is running:
  Ctrl+Alt+Shift+B

Or double-click Emergency Recover.cmd.

FOR THE DEVELOPER / DISTRIBUTOR
-------------------------------
On the development PC only:

  1. Run build.bat
  2. Confirm these exist:
       build\Esp32DisplayPowerAgent.exe
       build\Esp32DisplayPowerBridge.exe
  3. Run MakeRelease.bat

MakeRelease.bat does NOT compile anything. It packages the binaries you already
built into:

  release\ESP32BrightnessBridge_v10_2_PrecompiledSetup.zip

Give that generated ZIP to the recipient.

Do not give the source/developer folder to the end user unless you intentionally
want to. The generated release ZIP contains only the precompiled application and
installation/recovery files needed by the user.
