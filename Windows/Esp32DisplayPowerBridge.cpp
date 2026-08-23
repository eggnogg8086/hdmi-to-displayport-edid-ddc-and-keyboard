#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <powrprof.h>
#include <powersetting.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Dxva2.lib")
#pragma comment(lib, "PowrProf.lib")

static constexpr wchar_t kServiceName[] = L"ESP32DisplayPowerBridge";
static constexpr wchar_t kTargetPnpId[] = L"AUOD0A2";
static constexpr BYTE kVcpBrightness = 0x10;

// GUID_CONSOLE_DISPLAY_STATE = 6FE69556-704A-47A0-8F24-C28D936FDA47
static const GUID kGuidConsoleDisplayState =
{ 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

// GUID_SESSION_DISPLAY_STATUS = 2B84C20E-AD23-4DDF-93DB-05FFBD7EFCA5
// This is the preferred display-state notification for interactive user-mode apps.
static const GUID kGuidSessionDisplayStatus =
{ 0x2b84c20e, 0xad23, 0x4ddf, { 0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5 } };

// GUID_SESSION_USER_PRESENCE = 3C0F4548-C03F-4C4D-B9F2-237EDE686376
// Interactive-session user-presence notification. 0 = present, 1 = not present,
// 2 = inactive. Used only as a secondary wake signal after unattended resume.
static const GUID kGuidSessionUserPresence =
{ 0x3c0f4548, 0xc03f, 0x4c4d, { 0xb9, 0xf2, 0x23, 0x7e, 0xde, 0x68, 0x63, 0x76 } };

// GUID_MONITOR_POWER_ON = 02731015-4510-4526-99E6-E5A17EBD1AEA
// Older fallback notification; useful for comparing event timing on this machine.
static const GUID kGuidMonitorPowerOn =
{ 0x02731015, 0x4510, 0x4526, { 0x99, 0xe6, 0xe5, 0xa1, 0x7e, 0xbd, 0x1a, 0xea } };

// GUID_LIDSWITCH_STATE_CHANGE = BA3E0F4D-B817-4094-A2D1-D56379E6A0F3
// 0 = lid closed, 1 = lid open.  This makes the converted panel behave like
// an integrated laptop panel even when the configured lid action is "Do nothing".
static const GUID kGuidLidSwitchStateChange =
{ 0xba3e0f4d, 0xb817, 0x4094, { 0xa2, 0xd1, 0xd5, 0x63, 0x79, 0xe6, 0xa0, 0xf3 } };


// Display power-plan subgroup = 7516B95F-F776-4464-8C53-06167F40CC99
static const GUID kGuidVideoSubgroup =
{ 0x7516b95f, 0xf776, 0x4464, { 0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99 } };

// VIDEOIDLE / "Turn off display after" = 3C0BC021-C8A8-4E07-A973-6B14CBCB2B7E
static const GUID kGuidVideoIdle =
{ 0x3c0bc021, 0xc8a8, 0x4e07, { 0xa9, 0x73, 0x6b, 0x14, 0xcb, 0xcb, 0x2b, 0x7e } };

// Laptop-like timeout sequence with an HDMI hold:
//   T-8.0 s: after confirming no external ES_DISPLAY_REQUIRED request, acquire
//            PowerRequestDisplayRequired so Windows cannot tear down
//            the picture while our backlight sequence is running.
//   T-5.0 s: temporarily dim to 50% of the saved user brightness.
//   T-2.5 s: send VCP 0x10 = 0 so the ESP32 starts its local fade.
//   +1.2 s : after the OFF command, release the display power request.
//            At that point the backlight is already black, so Windows may
//            remove the HDMI picture whenever its own idle logic decides to.
static constexpr DWORD kDisplayHoldLeadMs = 5500;
static constexpr DWORD kDimLeadMs = 5000;
static constexpr DWORD kOffLeadMs = 2500;
static constexpr DWORD kFadeGuardAfterOffMs = 1200;
static constexpr DWORD kPreblankOffConfirmGraceMs = 2000;
static constexpr DWORD kSuspendFadeGuardMs = 1000;

static constexpr UINT_PTR kAgentTimerId = 1;
static constexpr UINT kAgentTimerPeriodMs = 100;
static constexpr ULONGLONG kBrightnessCacheIntervalMs = 5000;
static constexpr ULONGLONG kPowerPlanRefreshIntervalMs = 5000;
static constexpr ULONGLONG kStartupRecoveryRecentInputMs = 30000;
static constexpr ULONGLONG kLogRotateBytes = 2ULL * 1024ULL * 1024ULL;
static constexpr int kPanicHotkeyId = 0x4553;

static SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
static SERVICE_STATUS g_serviceStatus{};
static HANDLE g_stopEvent = nullptr;
static HANDLE g_workEvent = nullptr;
static HPOWERNOTIFY g_powerNotify = nullptr;
static std::atomic<LONG> g_pendingDisplayState{ -1 }; // -1 none, 0 off, 1 on, 2 dim
static std::atomic<bool> g_displayAssumedOn{ true };

static std::wstring ToUpper(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towupper(c));
    });
    return s;
}

static std::wstring GetLogDirectory()
{
    wchar_t programData[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? programData : L"C:\\ProgramData";
    return base + L"\\ESP32BrightnessBridge";
}

static std::wstring GetLogPath()
{
    return GetLogDirectory() + L"\\powerbridge.log";
}

static void EnsureLogDirectory()
{
    CreateDirectoryW(GetLogDirectory().c_str(), nullptr);
}

static void Log(const wchar_t* format, ...)
{
    EnsureLogDirectory();

    wchar_t msg[2048]{};
    va_list ap;
    va_start(ap, format);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, format, ap);
    va_end(ap);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t line[2300]{};
    _snwprintf_s(
        line,
        _countof(line),
        _TRUNCATE,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %s\r\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        msg
    );

    const std::wstring logPath = GetLogPath();
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &fad)) {
        const ULONGLONG size =
            (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (size >= kLogRotateBytes) {
            const std::wstring oldPath = logPath + L".1";
            DeleteFileW(oldPath.c_str());
            MoveFileExW(logPath.c_str(), oldPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
    }

    HANDLE h = CreateFileW(
        logPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (h != INVALID_HANDLE_VALUE) {
        int bytesNeeded = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
        if (bytesNeeded > 1) {
            std::vector<char> utf8(static_cast<size_t>(bytesNeeded));
            WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytesNeeded, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(h, utf8.data(), static_cast<DWORD>(bytesNeeded - 1), &written, nullptr);
        }
        CloseHandle(h);
    }
}

static DWORD LoadSavedBrightness()
{
    DWORD value = 50;
    DWORD size = sizeof(value);
    RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\ESP32BrightnessBridge",
        L"CurrentBrightness",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size
    );

    if (value == 0 || value > 100) {
        value = 50;
    }
    return value;
}

static void SaveBrightness(DWORD value)
{
    if (value == 0 || value > 100) {
        return;
    }

    HKEY key = nullptr;
    DWORD disposition = 0;
    LONG rc = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\ESP32BrightnessBridge",
        0,
        nullptr,
        0,
        KEY_SET_VALUE,
        nullptr,
        &key,
        &disposition
    );

    if (rc == ERROR_SUCCESS) {
        RegSetValueExW(
            key,
            L"CurrentBrightness",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&value),
            sizeof(value)
        );
        RegCloseKey(key);
    }
}

struct DisplayTarget
{
    std::wstring gdiName;
    std::wstring monitorDevicePath;
    std::wstring friendlyName;
};

static bool FindTargetDisplay(DisplayTarget& out)
{
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (rc != ERROR_SUCCESS) {
        Log(L"GetDisplayConfigBufferSizes failed: %ld", rc);
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

    rc = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS,
        &pathCount,
        paths.data(),
        &modeCount,
        modes.data(),
        nullptr
    );

    if (rc != ERROR_SUCCESS) {
        Log(L"QueryDisplayConfig failed: %ld", rc);
        return false;
    }

    const std::wstring needle = ToUpper(kTargetPnpId);

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[i].targetInfo.adapterId;
        target.header.id = paths[i].targetInfo.id;

        rc = DisplayConfigGetDeviceInfo(&target.header);
        if (rc != ERROR_SUCCESS) {
            continue;
        }

        std::wstring devicePath = target.monitorDevicePath;
        std::wstring friendly = target.monitorFriendlyDeviceName;
        std::wstring haystack = ToUpper(devicePath + L" " + friendly);

        if (haystack.find(needle) == std::wstring::npos) {
            continue;
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;

        rc = DisplayConfigGetDeviceInfo(&source.header);
        if (rc != ERROR_SUCCESS) {
            Log(L"DisplayConfigGetDeviceInfo(source) failed: %ld", rc);
            continue;
        }

        out.gdiName = source.viewGdiDeviceName;
        out.monitorDevicePath = devicePath;
        out.friendlyName = friendly;
        return true;
    }

    Log(L"Active target containing %s not found", kTargetPnpId);
    return false;
}

struct MonitorSearchContext
{
    std::wstring gdiName;
    HMONITOR hMonitor = nullptr;
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<MonitorSearchContext*>(lParam);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);

    if (!GetMonitorInfoW(hMonitor, &mi)) {
        return TRUE;
    }

    if (_wcsicmp(mi.szDevice, ctx->gdiName.c_str()) == 0) {
        ctx->hMonitor = hMonitor;
        return FALSE;
    }

    return TRUE;
}

class PhysicalMonitorSet
{
public:
    ~PhysicalMonitorSet()
    {
        if (!monitors_.empty()) {
            DestroyPhysicalMonitors(static_cast<DWORD>(monitors_.size()), monitors_.data());
        }
    }

    bool OpenForTarget(const DisplayTarget& target)
    {
        MonitorSearchContext ctx{};
        ctx.gdiName = target.gdiName;

        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));
        if (!ctx.hMonitor) {
            Log(L"No HMONITOR matched GDI source %s", target.gdiName.c_str());
            return false;
        }

        DWORD count = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(ctx.hMonitor, &count) || count == 0) {
            Log(L"GetNumberOfPhysicalMonitorsFromHMONITOR(%s) failed: %lu",
                target.gdiName.c_str(), GetLastError());
            return false;
        }

        monitors_.resize(count);
        if (!GetPhysicalMonitorsFromHMONITOR(ctx.hMonitor, count, monitors_.data())) {
            Log(L"GetPhysicalMonitorsFromHMONITOR failed: %lu", GetLastError());
            monitors_.clear();
            return false;
        }

        return true;
    }

    bool GetBrightness(DWORD& current, DWORD& maximum)
    {
        for (auto& pm : monitors_) {
            MC_VCP_CODE_TYPE vcpType = MC_MOMENTARY;
            DWORD cur = 0;
            DWORD max = 0;
            SetLastError(ERROR_SUCCESS);
            if (GetVCPFeatureAndVCPFeatureReply(
                    pm.hPhysicalMonitor,
                    kVcpBrightness,
                    &vcpType,
                    &cur,
                    &max)) {
                current = cur;
                maximum = max;
                static DWORD s_lastLoggedCurrent = 0xFFFFFFFFu;
                static DWORD s_lastLoggedMaximum = 0xFFFFFFFFu;
                if (cur != s_lastLoggedCurrent || max != s_lastLoggedMaximum) {
                    Log(L"DDC brightness read from '%s': current=%lu max=%lu",
                        pm.szPhysicalMonitorDescription, cur, max);
                    s_lastLoggedCurrent = cur;
                    s_lastLoggedMaximum = max;
                }
                return true;
            }
        }

        Log(L"VCP 0x10 read failed on all physical monitors (last error=%lu)", GetLastError());
        return false;
    }

    bool SetBrightness(DWORD value)
    {
        for (auto& pm : monitors_) {
            SetLastError(ERROR_SUCCESS);
            if (SetVCPFeature(pm.hPhysicalMonitor, kVcpBrightness, value)) {
                Log(L"DDC brightness write to '%s': %lu",
                    pm.szPhysicalMonitorDescription, value);
                return true;
            }
        }

        Log(L"VCP 0x10 write=%lu failed on all physical monitors (last error=%lu)",
            value, GetLastError());
        return false;
    }

private:
    std::vector<PHYSICAL_MONITOR> monitors_;
};

static bool OpenTargetPhysicalMonitors(DisplayTarget& target, PhysicalMonitorSet& set)
{
    if (!FindTargetDisplay(target)) {
        return false;
    }

    return set.OpenForTarget(target);
}

static bool ReadCurrentBrightness(DWORD& current, DWORD& maximum)
{
    DisplayTarget target;
    PhysicalMonitorSet set;
    if (!OpenTargetPhysicalMonitors(target, set)) {
        return false;
    }
    return set.GetBrightness(current, maximum);
}

static bool WriteBrightness(DWORD value)
{
    DisplayTarget target;
    PhysicalMonitorSet set;
    if (!OpenTargetPhysicalMonitors(target, set)) {
        return false;
    }
    return set.SetBrightness(value);
}

static void CacheBrightnessIfAvailable()
{
    DWORD current = 0;
    DWORD maximum = 0;
    if (!ReadCurrentBrightness(current, maximum)) {
        return;
    }

    if (current > 0 && current <= 100) {
        const DWORD previous = LoadSavedBrightness();
        if (current != previous) {
            SaveBrightness(current);
            Log(L"Cached nonzero brightness changed: %lu -> %lu", previous, current);
        }
    }
}

static DWORD GetTemporaryDimBrightness()
{
    const DWORD saved = LoadSavedBrightness();

    // 50% of the user's normal brightness, rounded up so a very low
    // nonzero setting never becomes an accidental OFF command.
    DWORD dimmed = (saved + 1u) / 2u;
    if (dimmed == 0) {
        dimmed = 1;
    }
    return dimmed;
}

static bool HandleDisplayDim()
{
    const DWORD saved = LoadSavedBrightness();
    const DWORD dimmed = GetTemporaryDimBrightness();

    Log(L"DISPLAY DIM -> temporary 50%% level: saved=%lu%% dim=%lu%%",
        saved, dimmed);

    // Do not call SaveBrightness() here. This is a temporary laptop-style
    // dim stage; the original nonzero user brightness must remain the wake
    // restore level.
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (WriteBrightness(dimmed)) {
            Log(L"DISPLAY DIM -> VCP 0x10 = %lu succeeded (attempt %d)",
                dimmed, attempt);
            return true;
        }
        Sleep(20);
    }

    Log(L"DISPLAY DIM -> VCP 0x10 = %lu failed after retries", dimmed);
    return false;
}

static bool HandleDisplayOff()
{
    Log(L"DISPLAY OFF event -> FAST VCP write");

    /*
     * IMPORTANT: do NOT perform a DDC read here.
     *
     * Windows may already be tearing down the HDMI link when the OFF notification
     * arrives.  The old v9 path first tried GetVCPFeature(), which can consume the
     * small window in which SetVCPFeature() still works.  The normal ON-state poll
     * already keeps the restore brightness cached, so send the one critical OFF
     * command immediately.
     */
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (WriteBrightness(0)) {
            Log(L"DISPLAY OFF -> VCP 0x10 = 0 succeeded (attempt %d)", attempt);
            return true;
        }
        Sleep(20);
    }

    Log(L"DISPLAY OFF -> VCP 0x10 = 0 failed after fast retries");
    return false;
}

static void HandleDisplayOn()
{
    DWORD restore = LoadSavedBrightness();
    Log(L"DISPLAY ON event -> restoring %lu%%", restore);

    // HDMI/DDC may not be ready at the instant Windows reports ON.
    for (int attempt = 1; attempt <= 20; ++attempt) {
        if (g_stopEvent && WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) {
            return;
        }

        if (WriteBrightness(restore)) {
            Log(L"DISPLAY ON -> restored %lu%% (attempt %d)", restore, attempt);
            return;
        }

        Sleep(500);
    }

    Log(L"DISPLAY ON -> restore failed after 10 seconds");
}

static DWORD WINAPI WorkerThreadProc(LPVOID)
{
    Log(L"Worker started; saved brightness=%lu", LoadSavedBrightness());

    // Prime the saved value from the ESP32 if the display is currently reachable.
    CacheBrightnessIfAvailable();

    HANDLE waits[2] = { g_stopEvent, g_workEvent };

    while (true) {
        DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 2000);

        if (wait == WAIT_OBJECT_0) {
            break;
        }

        if (wait == WAIT_OBJECT_0 + 1) {
            LONG state = g_pendingDisplayState.exchange(-1);

            if (state == 0) {
                g_displayAssumedOn.store(false);
                HandleDisplayOff();
            } else if (state == 1) {
                g_displayAssumedOn.store(true);
                HandleDisplayOn();
            } else if (state == 2) {
                Log(L"DISPLAY DIM event (no forced brightness change)");
            }
            continue;
        }

        if (wait == WAIT_TIMEOUT && g_displayAssumedOn.load()) {
            // Keep the restore level synchronized with Fn-key changes reported by ESP32 DDC/CI.
            CacheBrightnessIfAvailable();
        }
    }

    Log(L"Worker stopped");
    return 0;
}

static void QueueDisplayState(DWORD state)
{
    if (state > 2) {
        return;
    }

    g_pendingDisplayState.store(static_cast<LONG>(state));
    if (g_workEvent) {
        SetEvent(g_workEvent);
    }
}

static DWORD WINAPI ServiceControlHandler(
    DWORD control,
    DWORD eventType,
    LPVOID eventData,
    LPVOID)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        if (g_stopEvent) {
            SetEvent(g_stopEvent);
        }
        return NO_ERROR;

    case SERVICE_CONTROL_POWEREVENT:
        if (eventType == PBT_POWERSETTINGCHANGE && eventData) {
            const auto* setting = reinterpret_cast<POWERBROADCAST_SETTING*>(eventData);
            if (IsEqualGUID(setting->PowerSetting, kGuidConsoleDisplayState) &&
                setting->DataLength >= sizeof(DWORD)) {
                DWORD state = *reinterpret_cast<const DWORD*>(setting->Data);
                Log(L"GUID_CONSOLE_DISPLAY_STATE notification: %lu", state);
                QueueDisplayState(state);
            }
        }
        return NO_ERROR;

    default:
        return NO_ERROR;
    }
}

/* -------------------------------------------------------------------------
 * Interactive user-session power agent
 * ------------------------------------------------------------------------- */

static HPOWERNOTIFY g_agentSessionNotify = nullptr;
static HPOWERNOTIFY g_agentUserPresenceNotify = nullptr;
static HPOWERNOTIFY g_agentConsoleNotify = nullptr;
static HPOWERNOTIFY g_agentLegacyNotify = nullptr;
static HPOWERNOTIFY g_agentLidNotify = nullptr;
static HPOWERNOTIFY g_agentSuspendResumeNotify = nullptr;
static bool g_agentDisplayOn = true;
static bool g_agentDimmed = false;
static bool g_agentPreblanked = false;
static HANDLE g_agentDisplayPowerRequest = INVALID_HANDLE_VALUE;
static bool g_agentDisplayPowerRequestActive = false;
static DWORD g_agentDisplayHoldInputTick = 0;
static ULONGLONG g_agentDisplayHoldReleaseTick = 0;
static bool g_agentPreblankSuppressedUntilInput = false;
static DWORD g_agentSuppressionInputTick = 0;
static DWORD g_agentPreblankInputTick = 0;
static ULONGLONG g_agentExpectedWindowsOffTick = 0;

// After an automatic VIDEOIDLE off, keep the panel latched dark until there is
// actual user input. Display power notifications alone are not sufficient: the
// PowerRequest hold/release sequence and legacy monitor notifications can
// generate an ON transition even though nobody touched the machine.
static bool g_agentAwaitingUserWake = false;
static DWORD g_agentAutoOffInputTick = 0;
static ULONGLONG g_agentLastBrightnessCacheTick = 0;
static ULONGLONG g_agentLastPowerPlanRefreshTick = 0;
static DWORD g_agentDisplayIdleTimeoutSec = 0;
static bool g_agentDisplayIdleOnAc = true;

// True only after an unattended/automatic resume. While set, display-ON
// notifications alone are not allowed to light the panel. A real user-presence
// transition or a later PBT_APMRESUMESUSPEND clears it.
static bool g_agentResumeWaitingForUserPresence = false;

// Set when this agent actually receives PBT_APMSUSPEND.  While set, an
// authoritative GUID_SESSION_DISPLAY_STATUS=ON means Windows has chosen to
// light the user's session again (for example a physical power-button wake).
// That is a better wake signal than GetLastInputInfo(), which power-button
// resumes do not necessarily update.
static bool g_agentWasSuspended = false;

// v9.9: Windows applications can keep the display awake without generating
// keyboard/mouse input.  Multimedia applications normally do this with
// ES_DISPLAY_REQUIRED / PowerRequestDisplayRequired.  GetLastInputInfo() alone
// therefore cannot be used as proof that Windows intends to blank the display.
//
// We sample SystemExecutionState only while our own display hold is NOT active,
// so ES_DISPLAY_REQUIRED means another application/system component is asking
// Windows to keep the display on rather than our own fade guard.
static bool g_agentExternalDisplayRequired = false;
static ULONGLONG g_agentPostRequestOffTick = 0;
static bool g_agentExecutionStateErrorLogged = false;
static bool g_agentStartupRecoveryPending = true;
static bool g_agentPredictionUnavailableLogged = false;
static bool g_agentLidClosed = false;

static bool EnsureAgentDisplayPowerRequest()
{
    if (g_agentDisplayPowerRequest != INVALID_HANDLE_VALUE) {
        return true;
    }

    REASON_CONTEXT reason{};
    reason.Version = POWER_REQUEST_CONTEXT_VERSION;
    reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    reason.Reason.SimpleReasonString =
        const_cast<LPWSTR>(L"ESP32 panel fade before Windows display power-off");

    g_agentDisplayPowerRequest = PowerCreateRequest(&reason);
    if (g_agentDisplayPowerRequest == INVALID_HANDLE_VALUE) {
        Log(L"PowerCreateRequest failed: %lu", GetLastError());
        return false;
    }

    Log(L"Created display power-request handle=%p", g_agentDisplayPowerRequest);
    return true;
}

static bool AcquireAgentDisplayHold(DWORD lastInputTick)
{
    if (g_agentDisplayPowerRequestActive) {
        return true;
    }

    if (!EnsureAgentDisplayPowerRequest()) {
        return false;
    }

    if (!PowerSetRequest(g_agentDisplayPowerRequest, PowerRequestDisplayRequired)) {
        Log(L"PowerSetRequest(PowerRequestDisplayRequired) failed: %lu", GetLastError());
        return false;
    }

    g_agentDisplayPowerRequestActive = true;
    g_agentDisplayHoldInputTick = lastInputTick;
    g_agentDisplayHoldReleaseTick = 0;
    Log(L"DISPLAY HOLD acquired -> Windows must keep the session display pipeline on");
    return true;
}

static void ReleaseAgentDisplayHold(const wchar_t* reason)
{
    if (!g_agentDisplayPowerRequestActive) {
        g_agentDisplayHoldReleaseTick = 0;
        return;
    }

    if (!PowerClearRequest(g_agentDisplayPowerRequest, PowerRequestDisplayRequired)) {
        const DWORD err = GetLastError();
        Log(L"PowerClearRequest(PowerRequestDisplayRequired) failed: %lu; closing request handle", err);

        // Closing a power-request object releases any request counts that belong
        // to it. Recreate the object next time rather than risk leaving Windows
        // artificially held on.
        CloseHandle(g_agentDisplayPowerRequest);
        g_agentDisplayPowerRequest = INVALID_HANDLE_VALUE;
    } else {
        Log(L"DISPLAY HOLD released: %s", reason ? reason : L"no reason");
    }

    g_agentDisplayPowerRequestActive = false;
    g_agentDisplayHoldReleaseTick = 0;
}

static void CloseAgentDisplayPowerRequest()
{
    ReleaseAgentDisplayHold(L"agent shutdown");
    if (g_agentDisplayPowerRequest != INVALID_HANDLE_VALUE) {
        CloseHandle(g_agentDisplayPowerRequest);
        g_agentDisplayPowerRequest = INVALID_HANDLE_VALUE;
    }
}

static bool QueryDisplayIdleTimeout(DWORD& timeoutSeconds, bool& onAcPower)
{
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status)) {
        Log(L"GetSystemPowerStatus failed: %lu", GetLastError());
        return false;
    }

    onAcPower = (status.ACLineStatus != 0);

    GUID* activeScheme = nullptr;
    DWORD rc = PowerGetActiveScheme(nullptr, &activeScheme);
    if (rc != ERROR_SUCCESS || !activeScheme) {
        Log(L"PowerGetActiveScheme failed: %lu", rc);
        return false;
    }

    DWORD value = 0;
    if (onAcPower) {
        rc = PowerReadACValueIndex(nullptr, activeScheme, &kGuidVideoSubgroup, &kGuidVideoIdle, &value);
    } else {
        rc = PowerReadDCValueIndex(nullptr, activeScheme, &kGuidVideoSubgroup, &kGuidVideoIdle, &value);
    }

    LocalFree(activeScheme);

    if (rc != ERROR_SUCCESS) {
        Log(L"PowerRead%sValueIndex(VIDEOIDLE) failed: %lu",
            onAcPower ? L"AC" : L"DC", rc);
        return false;
    }

    timeoutSeconds = value;
    return true;
}

static bool QuerySystemExecutionState(EXECUTION_STATE& state)
{
    ULONG rawState = 0;
    // CallNtPowerInformation returns NTSTATUS, whose Win32 ABI is a signed
    // 32-bit LONG.  Some Windows SDK configurations do not expose the
    // NTSTATUS typedef to ordinary desktop C++ translation units, so keep
    // the value in LONG instead of depending on ntstatus.h.
    const LONG status = CallNtPowerInformation(
        SystemExecutionState,
        nullptr,
        0,
        &rawState,
        sizeof(rawState));

    if (status != 0) {
        if (!g_agentExecutionStateErrorLogged) {
            Log(L"CallNtPowerInformation(SystemExecutionState) failed: 0x%08lX",
                static_cast<unsigned long>(status));
            g_agentExecutionStateErrorLogged = true;
        }
        return false;
    }

    g_agentExecutionStateErrorLogged = false;
    state = static_cast<EXECUTION_STATE>(rawState);
    return true;
}

static bool GetSessionIdleState(DWORD& idleMs, DWORD& lastInputTick)
{
    LASTINPUTINFO lii{};
    lii.cbSize = sizeof(lii);

    if (!GetLastInputInfo(&lii)) {
        Log(L"GetLastInputInfo failed: %lu", GetLastError());
        return false;
    }

    const DWORD now32 = GetTickCount();
    lastInputTick = lii.dwTime;
    idleMs = now32 - lii.dwTime;
    return true;
}

static void RefreshDisplayIdlePolicy(bool forceLog)
{
    DWORD seconds = 0;
    bool onAc = true;
    if (!QueryDisplayIdleTimeout(seconds, onAc)) {
        return;
    }

    if (forceLog ||
        seconds != g_agentDisplayIdleTimeoutSec ||
        onAc != g_agentDisplayIdleOnAc) {
        Log(L"Windows VIDEOIDLE policy: %lu seconds (%s)",
            seconds, onAc ? L"AC" : L"battery");
    }

    g_agentDisplayIdleTimeoutSec = seconds;
    g_agentDisplayIdleOnAc = onAc;
}

static void RestoreAfterCancelledPreblank(const wchar_t* reason, DWORD currentInputTick)
{
    Log(L"EARLY BLANK cancelled: %s -> restoring saved brightness", reason);

    g_agentDimmed = false;
    g_agentPreblanked = false;
    g_agentAwaitingUserWake = false;
    g_agentAutoOffInputTick = 0;
    g_agentExpectedWindowsOffTick = 0;
    g_agentPostRequestOffTick = 0;
    g_agentPreblankSuppressedUntilInput = true;
    g_agentSuppressionInputTick = currentInputTick;

    ReleaseAgentDisplayHold(L"early sequence cancelled");
    HandleDisplayOn();
}

static void AgentTimerTick()
{
    const ULONGLONG now = GetTickCount64();

    if (g_agentLastPowerPlanRefreshTick == 0 ||
        now - g_agentLastPowerPlanRefreshTick >= kPowerPlanRefreshIntervalMs) {
        RefreshDisplayIdlePolicy(g_agentLastPowerPlanRefreshTick == 0);
        g_agentLastPowerPlanRefreshTick = now;
    }

    DWORD idleMs = 0;
    DWORD lastInputTick = 0;
    if (!GetSessionIdleState(idleMs, lastInputTick)) {
        return;
    }

    const ULONGLONG timeoutMs =
        static_cast<ULONGLONG>(g_agentDisplayIdleTimeoutSec) * 1000ULL;

    // Fail-safe after an agent crash/update: if the process has just started and
    // this session has recent real input, make sure the panel is visible.  A
    // stale VCP 0x10=0 must never strand the user at a black desktop.
    if (g_agentStartupRecoveryPending) {
        g_agentStartupRecoveryPending = false;
        if (static_cast<ULONGLONG>(idleMs) <= kStartupRecoveryRecentInputMs) {
            Log(L"Startup recovery: recent user input detected (%lu ms idle) -> forcing saved brightness ON", idleMs);
            g_agentDisplayOn = true;
            g_agentDimmed = false;
            g_agentPreblanked = false;
            g_agentAwaitingUserWake = false;
            HandleDisplayOn();
        } else {
            Log(L"Startup recovery skipped: session idle=%lu ms", idleMs);
        }
    }

    // Respect Windows display-availability requests.  A video player can keep
    // the display on with ES_DISPLAY_REQUIRED while GetLastInputInfo() continues
    // to age.  v9.8 ignored that distinction and repeatedly entered the
    // DIM/OFF sequence during video playback.
    //
    // Do not query while our own PowerRequestDisplayRequired hold is active:
    // the aggregate SystemExecutionState would then include our own request and
    // could not be distinguished from another application's request.
    bool executionStateKnown = false;
    bool externalDisplayRequiredNow = false;
    if (!g_agentDisplayPowerRequestActive) {
        EXECUTION_STATE executionState = 0;
        if (QuerySystemExecutionState(executionState)) {
            executionStateKnown = true;
            externalDisplayRequiredNow =
                (executionState & ES_DISPLAY_REQUIRED) != 0;
        }
    }

    if (!g_agentDisplayPowerRequestActive && !executionStateKnown) {
        if (!g_agentPredictionUnavailableLogged) {
            Log(L"Predictive VIDEOIDLE disabled for this tick: Windows execution state is unknown; failing open");
            g_agentPredictionUnavailableLogged = true;
        }
    } else if (executionStateKnown) {
        g_agentPredictionUnavailableLogged = false;
    }

    if (executionStateKnown && externalDisplayRequiredNow) {
        if (!g_agentExternalDisplayRequired) {
            Log(L"External ES_DISPLAY_REQUIRED detected -> suspending predictive VIDEOIDLE dim/off");
        }
        g_agentExternalDisplayRequired = true;
        g_agentPostRequestOffTick = 0;

        // If we have not yet intentionally turned the panel off, an external
        // display request wins.  This is normally reached before Stage 0 because
        // our own hold has not been acquired yet.
        if ((g_agentDimmed || g_agentPreblanked) && !g_agentAwaitingUserWake) {
            Log(L"External display request arrived during predictive sequence -> restoring normal brightness");
            g_agentDimmed = false;
            g_agentPreblanked = false;
            g_agentExpectedWindowsOffTick = 0;
            ReleaseAgentDisplayHold(L"external display-required request");
            HandleDisplayOn();
        }
    } else if (executionStateKnown && g_agentExternalDisplayRequired &&
               !externalDisplayRequiredNow) {
        Log(L"External ES_DISPLAY_REQUIRED cleared");
        g_agentExternalDisplayRequired = false;

        // If the application held the display awake past the configured Windows
        // timeout, Windows may become eligible to blank immediately when the
        // request disappears.  Take our short fade guard immediately, dim now,
        // then send OFF after the usual 2.5 s stage.
        if (g_agentDisplayOn && !g_agentAwaitingUserWake && timeoutMs != 0 &&
            static_cast<ULONGLONG>(idleMs) >= timeoutMs) {
            if (AcquireAgentDisplayHold(lastInputTick)) {
                if (HandleDisplayDim()) {
                    g_agentDimmed = true;
                    g_agentPreblankInputTick = lastInputTick;
                    g_agentPostRequestOffTick = now + kOffLeadMs;
                    Log(L"Display request ended after VIDEOIDLE deadline -> DIM 50%% now; OFF in %lu ms",
                        kOffLeadMs);
                }
            }
        }
    }

    // A timeout-driven OFF is latched until *real* user input occurs. This is
    // deliberately checked independently of Windows display-state notifications
    // so a synthetic/stale ON notification cannot wake the backlight.
    if (g_agentAwaitingUserWake && lastInputTick != g_agentAutoOffInputTick) {
        Log(L"Real user input observed after automatic display-off -> restoring panel");
        g_agentAwaitingUserWake = false;
        g_agentAutoOffInputTick = 0;
        g_agentDisplayOn = true;
        g_agentDimmed = false;
        g_agentPreblanked = false;
        g_agentPreblankSuppressedUntilInput = false;
        g_agentExpectedWindowsOffTick = 0;
        g_agentPostRequestOffTick = 0;
        ReleaseAgentDisplayHold(L"real user wake");
        HandleDisplayOn();
        return;
    }

    // Once the ESP32 has had enough time to finish its local fade, release
    // Windows' display hold. If Windows' own idle deadline has already passed,
    // it can now remove the HDMI picture -- behind an already-black backlight.
    if (g_agentDisplayPowerRequestActive &&
        g_agentDisplayHoldReleaseTick != 0 &&
        now >= g_agentDisplayHoldReleaseTick) {
        ReleaseAgentDisplayHold(L"ESP32 fade guard complete");
    }

    // If we only acquired the hold so far and the user becomes active again,
    // drop it immediately. No brightness restoration is needed until DIM/OFF.
    if (g_agentDisplayPowerRequestActive &&
        !g_agentDimmed &&
        !g_agentPreblanked &&
        lastInputTick != g_agentDisplayHoldInputTick) {
        ReleaseAgentDisplayHold(L"user input before dim stage");
    }

    if (g_agentPreblankSuppressedUntilInput &&
        lastInputTick != g_agentSuppressionInputTick) {
        Log(L"User input observed -> re-arming early VIDEOIDLE blanking");
        g_agentPreblankSuppressedUntilInput = false;
    }

    if (g_agentDimmed || g_agentPreblanked) {
        if (lastInputTick != g_agentPreblankInputTick) {
            RestoreAfterCancelledPreblank(
                L"user input during dim/off sequence",
                lastInputTick);
            return;
        }

        // Only abandon an uncompleted DIM stage. Once VCP 0x10=0 has
        // succeeded, never wake the panel merely because Windows failed to
        // produce an OFF notification on our predicted schedule. The display
        // hold itself can shift/suppress that notification, and v9.5's old
        // watchdog was the direct cause of spontaneous re-wake.
        if (g_agentDimmed && !g_agentPreblanked && !g_agentAwaitingUserWake &&
            g_agentExpectedWindowsOffTick != 0 &&
            now > g_agentExpectedWindowsOffTick + kPreblankOffConfirmGraceMs) {
            RestoreAfterCancelledPreblank(
                L"DIM stage expired before OFF command completed",
                lastInputTick);
            return;
        }
    }

    // Deferred OFF after an external display-required request ends beyond the
    // normal idle deadline.  The display hold is already active, so HDMI stays
    // alive while the ESP32 performs the same laptop-like fade sequence.
    if (g_agentPostRequestOffTick != 0 && now >= g_agentPostRequestOffTick &&
        !g_agentPreblanked) {
        if (lastInputTick != g_agentPreblankInputTick) {
            RestoreAfterCancelledPreblank(
                L"user input after display-required request ended",
                lastInputTick);
            return;
        }

        Log(L"POST-REQUEST VIDEOIDLE OFF -> sending VCP 0x10=0");
        if (HandleDisplayOff()) {
            g_agentPreblanked = true;
            g_agentAwaitingUserWake = true;
            g_agentAutoOffInputTick = lastInputTick;
            g_agentPostRequestOffTick = 0;
            if (g_agentDisplayPowerRequestActive) {
                g_agentDisplayHoldReleaseTick = now + kFadeGuardAfterOffMs;
            }
            Log(L"POST-REQUEST OFF succeeded; panel latched dark until real user wake");
            return;
        }

        Log(L"POST-REQUEST OFF DDC write failed; retrying on next timer tick");
    }

    if (!g_agentDisplayOn) {
        return;
    }

    if (!g_agentPreblankSuppressedUntilInput &&
        !g_agentExternalDisplayRequired &&
        timeoutMs != 0) {
        const ULONGLONG idle64 = idleMs;

        if (idle64 < timeoutMs) {
            const ULONGLONG remainingMs = timeoutMs - idle64;

            // Stage 0: acquire a short-lived DISPLAY_REQUIRED power request well
            // before the visible sequence. This is the key difference from v9.4:
            // even if Windows' internal display-idle clock is a few seconds ahead
            // of GetLastInputInfo(), the GPU/display path is held on until our
            // ESP32 fade has completed.
            if (!g_agentDisplayPowerRequestActive &&
                executionStateKnown &&
                !externalDisplayRequiredNow &&
                remainingMs <= kDisplayHoldLeadMs) {
                Log(L"EARLY VIDEOIDLE HOLD: idle=%lu ms timeout=%llu ms remaining=%llu ms lead=%lu ms",
                    idleMs,
                    static_cast<unsigned long long>(timeoutMs),
                    static_cast<unsigned long long>(remainingMs),
                    kDisplayHoldLeadMs);
                AcquireAgentDisplayHold(lastInputTick);
            }

            // Stage 1: start the integrated-panel-style temporary dim.
            if (g_agentDisplayPowerRequestActive &&
                !g_agentDimmed &&
                !g_agentPreblanked &&
                remainingMs <= kDimLeadMs) {

                Log(L"EARLY VIDEOIDLE DIM: idle=%lu ms timeout=%llu ms remaining=%llu ms lead=%lu ms",
                    idleMs,
                    static_cast<unsigned long long>(timeoutMs),
                    static_cast<unsigned long long>(remainingMs),
                    kDimLeadMs);

                if (HandleDisplayDim()) {
                    g_agentDimmed = true;
                    g_agentPreblankInputTick = lastInputTick;
                    g_agentExpectedWindowsOffTick = now + remainingMs;
                    Log(L"EARLY VIDEOIDLE DIM succeeded; holding at 50%% until OFF stage");
                } else {
                    Log(L"EARLY VIDEOIDLE DIM failed; OFF stage remains armed");
                }
            }

            // Stage 2: send OFF well before Windows removes the HDMI picture.
            if (g_agentDisplayPowerRequestActive &&
                !g_agentPreblanked &&
                remainingMs <= kOffLeadMs) {

                Log(L"EARLY VIDEOIDLE OFF: idle=%lu ms timeout=%llu ms remaining=%llu ms lead=%lu ms",
                    idleMs,
                    static_cast<unsigned long long>(timeoutMs),
                    static_cast<unsigned long long>(remainingMs),
                    kOffLeadMs);

                if (HandleDisplayOff()) {
                    g_agentPreblanked = true;
                    if (!g_agentDimmed) {
                        g_agentPreblankInputTick = lastInputTick;
                    }
                    // From this point forward, do not restore merely because a
                    // power notification says ON. Require LastInputInfo to move.
                    g_agentAwaitingUserWake = true;
                    g_agentAutoOffInputTick = lastInputTick;
                    g_agentExpectedWindowsOffTick = now + remainingMs;

                    if (g_agentDisplayPowerRequestActive) {
                        g_agentDisplayHoldReleaseTick = now + kFadeGuardAfterOffMs;
                        Log(L"EARLY VIDEOIDLE OFF succeeded; holding HDMI for another %lu ms while ESP32 fades",
                            kFadeGuardAfterOffMs);
                    } else {
                        Log(L"EARLY VIDEOIDLE OFF succeeded; WARNING: display hold was unavailable");
                    }
                    return;
                }

                Log(L"EARLY VIDEOIDLE OFF DDC write failed; Windows OFF notification remains fallback");
            }
        }
    }

    bool safeToPollBrightness = !g_agentDimmed && !g_agentPreblanked;
    if (timeoutMs != 0 && idleMs < timeoutMs) {
        const ULONGLONG remaining = timeoutMs - static_cast<ULONGLONG>(idleMs);
        if (remaining <= kDimLeadMs + 2000ULL) {
            safeToPollBrightness = false;
        }
    }

    if (safeToPollBrightness &&
        (g_agentLastBrightnessCacheTick == 0 ||
         now - g_agentLastBrightnessCacheTick >= kBrightnessCacheIntervalMs)) {
        CacheBrightnessIfAvailable();
        g_agentLastBrightnessCacheTick = now;
    }
}

static void AgentHandleSuspend()
{
    Log(L"PBT_APMSUSPEND -> forcing ESP32 backlight OFF before system suspend");

    g_agentWasSuspended = true;

    // Never allow our timeout-only display hold to interfere with an explicit
    // or system suspend transition. PBT_APMSUSPEND is the independent OFF path.
    ReleaseAgentDisplayHold(L"system suspend");

    g_agentDisplayOn = false;
    g_agentDimmed = false;
    g_agentPreblanked = false;
    g_agentAwaitingUserWake = false;
    g_agentAutoOffInputTick = 0;
    g_agentExpectedWindowsOffTick = 0;
    g_agentPostRequestOffTick = 0;
    g_agentExternalDisplayRequired = false;
    g_agentResumeWaitingForUserPresence = false;

    if (HandleDisplayOff()) {
        Log(L"Suspend OFF write succeeded; holding %lu ms for ESP32 local fade",
            kSuspendFadeGuardMs);
        Sleep(kSuspendFadeGuardMs);
    } else {
        Log(L"Suspend OFF write failed; system suspend continues");
    }
}

static void AgentHandleResume(const wchar_t* source)
{
    Log(L"%s -> system resumed; restoring display brightness", source);

    ReleaseAgentDisplayHold(L"system resume");
    g_agentDisplayOn = true;
    g_agentDimmed = false;
    g_agentPreblanked = false;
    g_agentAwaitingUserWake = false;
    g_agentAutoOffInputTick = 0;
    g_agentPreblankSuppressedUntilInput = false;
    g_agentExpectedWindowsOffTick = 0;
    g_agentPostRequestOffTick = 0;
    g_agentExternalDisplayRequired = false;
    g_agentResumeWaitingForUserPresence = false;
    g_agentWasSuspended = false;

    HandleDisplayOn();
}

static const wchar_t* PowerGuidName(const GUID& guid)
{
    if (IsEqualGUID(guid, kGuidSessionDisplayStatus)) return L"GUID_SESSION_DISPLAY_STATUS";
    if (IsEqualGUID(guid, kGuidConsoleDisplayState))  return L"GUID_CONSOLE_DISPLAY_STATE";
    if (IsEqualGUID(guid, kGuidMonitorPowerOn))       return L"GUID_MONITOR_POWER_ON";
    if (IsEqualGUID(guid, kGuidLidSwitchStateChange)) return L"GUID_LIDSWITCH_STATE_CHANGE";
    return L"UNKNOWN_POWER_GUID";
}

static void AgentApplyDisplayState(DWORD state, const wchar_t* source)
{
    Log(L"%s notification: %lu", source, state);

    if (state == 0) {
        const bool alreadyOff = g_agentPreblanked || g_agentAwaitingUserWake;

        ReleaseAgentDisplayHold(L"Windows confirmed display OFF");
        g_agentDisplayOn = false;
        g_agentDimmed = false;
        g_agentPreblanked = false;
        g_agentExpectedWindowsOffTick = 0;
        g_agentPostRequestOffTick = 0;

        if (alreadyOff) {
            // Preserve g_agentAwaitingUserWake and its LastInputInfo baseline.
            Log(L"Windows OFF confirmed after early OFF; panel remains latched dark until real user input");
        } else {
            DWORD idleMs = 0;
            DWORD lastInputTick = 0;
            if (GetSessionIdleState(idleMs, lastInputTick)) {
                g_agentAwaitingUserWake = true;
                g_agentAutoOffInputTick = lastInputTick;
            }
            HandleDisplayOff();
        }
        return;
    }

    if (state == 1) {
        /*
         * v9.9: after a real suspend, trust the authoritative session-display
         * ON transition as the user-visible wake signal.
         *
         * A physical power-button wake can turn the Windows session display on
         * without updating GetLastInputInfo(), and IsSystemResumeAutomatic()
         * may still report TRUE early in the resume sequence.  If Windows has
         * actually turned GUID_SESSION_DISPLAY_STATUS ON after we previously
         * received PBT_APMSUSPEND, restore immediately.
         *
         * Unattended maintenance wakes normally leave the session display OFF,
         * so they do not hit this path.
         */
        if (g_agentWasSuspended &&
            source &&
            _wcsicmp(source, L"GUID_SESSION_DISPLAY_STATUS") == 0) {
            Log(L"Authoritative SESSION display ON after suspend -> accepting power-button/user-visible wake");
            AgentHandleResume(L"GUID_SESSION_DISPLAY_STATUS after suspend");
            return;
        }

        // After an unattended/automatic resume, Windows can produce power/display
        // telemetry without a person actually waking the machine. Do not light the
        // panel until Windows says the resume is no longer automatic, user presence
        // becomes Present, or PBT_APMRESUMESUSPEND arrives.
        if (g_agentResumeWaitingForUserPresence) {
            if (IsSystemResumeAutomatic()) {
                Log(L"Ignoring display ON notification during unattended resume");
                g_agentDisplayOn = false;
                return;
            }

            Log(L"Display ON after resume is now user-active -> wake accepted");
            g_agentResumeWaitingForUserPresence = false;
        }

        if (g_agentAwaitingUserWake) {
            DWORD idleMs = 0;
            DWORD lastInputTick = 0;
            if (GetSessionIdleState(idleMs, lastInputTick) &&
                lastInputTick == g_agentAutoOffInputTick) {
                Log(L"Ignoring display ON notification: no user input since automatic OFF");
                g_agentDisplayOn = false;
                return;
            }

            Log(L"Display ON coincides with real user input -> wake accepted");
            g_agentAwaitingUserWake = false;
            g_agentAutoOffInputTick = 0;
        }

        // PowerRequestDisplayRequired can itself cause/maintain an ON state while
        // our intentional DIM stage is in progress.  That ON notification is not
        // a wake request and must not immediately undo the 50%% dim.
        if (g_agentDimmed && !g_agentPreblanked &&
            g_agentDisplayPowerRequestActive) {
            DWORD idleMs = 0;
            DWORD lastInputTick = 0;
            if (GetSessionIdleState(idleMs, lastInputTick) &&
                lastInputTick == g_agentPreblankInputTick) {
                Log(L"Ignoring display ON notification during intentional DIM/HDMI-hold sequence");
                g_agentDisplayOn = true;
                return;
            }
        }

        const bool needsRestore = !g_agentDisplayOn || g_agentDimmed || g_agentPreblanked;

        ReleaseAgentDisplayHold(L"Windows display ON");
        g_agentDisplayOn = true;
        g_agentDimmed = false;
        g_agentPreblanked = false;
        g_agentPreblankSuppressedUntilInput = false;
        g_agentExpectedWindowsOffTick = 0;
        g_agentPostRequestOffTick = 0;

        if (needsRestore) {
            HandleDisplayOn();
        }
        return;
    }

    if (state == 2) {
        if (g_agentExternalDisplayRequired) {
            Log(L"Ignoring DIM notification while external ES_DISPLAY_REQUIRED is active");
            return;
        }

        if (g_agentAwaitingUserWake) {
            Log(L"Ignoring DIM notification while automatic OFF is latched");
            return;
        }

        if (g_agentDisplayOn && !g_agentPreblanked && !g_agentDimmed) {
            DWORD idleMs = 0;
            DWORD lastInputTick = 0;
            GetSessionIdleState(idleMs, lastInputTick);

            if (HandleDisplayDim()) {
                g_agentDimmed = true;
                g_agentPreblankInputTick = lastInputTick;
                Log(L"DISPLAY DIM event from %s -> temporary 50%% brightness", source);
            }
        }
    }
}

static LRESULT CALLBACK AgentWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g_agentSessionNotify = RegisterPowerSettingNotification(
            hwnd, &kGuidSessionDisplayStatus, DEVICE_NOTIFY_WINDOW_HANDLE);
        g_agentUserPresenceNotify = RegisterPowerSettingNotification(
            hwnd, &kGuidSessionUserPresence, DEVICE_NOTIFY_WINDOW_HANDLE);
        g_agentConsoleNotify = RegisterPowerSettingNotification(
            hwnd, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
        g_agentLegacyNotify = RegisterPowerSettingNotification(
            hwnd, &kGuidMonitorPowerOn, DEVICE_NOTIFY_WINDOW_HANDLE);
        g_agentLidNotify = RegisterPowerSettingNotification(
            hwnd, &kGuidLidSwitchStateChange, DEVICE_NOTIFY_WINDOW_HANDLE);

        // Explicitly opt this hidden desktop process into suspend/resume
        // notifications. This is important on modern Windows / Modern Standby,
        // where background desktop processes should not rely only on the
        // generic WM_POWERBROADCAST delivery path.
        g_agentSuspendResumeNotify = RegisterSuspendResumeNotification(
            hwnd, DEVICE_NOTIFY_WINDOW_HANDLE);

        Log(L"Interactive agent registrations: session=%p presence=%p console=%p legacy=%p lid=%p suspendresume=%p",
            g_agentSessionNotify, g_agentUserPresenceNotify, g_agentConsoleNotify,
            g_agentLegacyNotify, g_agentLidNotify, g_agentSuspendResumeNotify);

        if (!g_agentSessionNotify) {
            Log(L"Register GUID_SESSION_DISPLAY_STATUS failed: %lu", GetLastError());
        }
        if (!g_agentUserPresenceNotify) {
            Log(L"Register GUID_SESSION_USER_PRESENCE failed: %lu", GetLastError());
        }
        if (!g_agentConsoleNotify) {
            Log(L"Register GUID_CONSOLE_DISPLAY_STATE failed: %lu", GetLastError());
        }
        if (!g_agentLegacyNotify) {
            Log(L"Register GUID_MONITOR_POWER_ON failed: %lu", GetLastError());
        }
        if (!g_agentLidNotify) {
            Log(L"Register GUID_LIDSWITCH_STATE_CHANGE failed: %lu", GetLastError());
        }
        if (!g_agentSuspendResumeNotify) {
            Log(L"RegisterSuspendResumeNotification failed: %lu", GetLastError());
        }

        SetTimer(hwnd, kAgentTimerId, kAgentTimerPeriodMs, nullptr);

        if (RegisterHotKey(hwnd, kPanicHotkeyId,
                MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'B')) {
            Log(L"Emergency recovery hotkey registered: Ctrl+Alt+Shift+B");
        } else {
            Log(L"WARNING: emergency recovery hotkey registration failed: %lu", GetLastError());
        }
        return 0;

    case WM_TIMER:
        if (wParam == kAgentTimerId) {
            AgentTimerTick();
        }
        return 0;

    case WM_HOTKEY:
        if (wParam == kPanicHotkeyId) {
            DWORD idleMs = 0;
            DWORD inputTick = 0;
            GetSessionIdleState(idleMs, inputTick);
            Log(L"EMERGENCY HOTKEY -> cancelling all automatic-off state and forcing saved brightness ON");
            ReleaseAgentDisplayHold(L"emergency hotkey");
            g_agentDisplayOn = true;
            g_agentDimmed = false;
            g_agentPreblanked = false;
            g_agentAwaitingUserWake = false;
            g_agentAutoOffInputTick = 0;
            g_agentPostRequestOffTick = 0;
            g_agentExternalDisplayRequired = false;
            g_agentPreblankSuppressedUntilInput = true;
            g_agentSuppressionInputTick = inputTick;
            HandleDisplayOn();
            return 0;
        }
        break;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND) {
            AgentHandleSuspend();
            return TRUE;
        }

        if (wParam == PBT_APMRESUMEAUTOMATIC) {
            // This event is sent for every resume. IsSystemResumeAutomatic() is the
            // critical distinction: FALSE means Windows considers the wake user-
            // initiated (including the physical power button); TRUE means an
            // unattended timer/device/maintenance wake. A power-button wake does
            // not necessarily update GetLastInputInfo(), so do not require a key or
            // touchpad event here.
            const BOOL automaticResume = IsSystemResumeAutomatic();
            Log(L"PBT_APMRESUMEAUTOMATIC received; IsSystemResumeAutomatic=%s",
                automaticResume ? L"TRUE" : L"FALSE");

            g_agentDisplayOn = false;

            if (!automaticResume) {
                g_agentResumeWaitingForUserPresence = false;
                AgentHandleResume(L"PBT_APMRESUMEAUTOMATIC user/power-button wake");
            } else {
                g_agentResumeWaitingForUserPresence = true;
                Log(L"Automatic resume reported; waiting for authoritative SESSION display ON, PBT_APMRESUMESUSPEND, or user presence");
            }
            return TRUE;
        }

        if (wParam == PBT_APMRESUMESUSPEND) {
            // Windows documents this as the user-interaction resume notification,
            // including power-button wake. Keep it as a second independent path in
            // case a platform reports IsSystemResumeAutomatic() conservatively.
            g_agentResumeWaitingForUserPresence = false;
            if (!g_agentDisplayOn) {
                AgentHandleResume(L"PBT_APMRESUMESUSPEND");
            } else {
                Log(L"PBT_APMRESUMESUSPEND received; display already restored");
            }
            return TRUE;
        }

        if (wParam == PBT_APMRESUMECRITICAL) {
            const BOOL automaticResume = IsSystemResumeAutomatic();
            Log(L"PBT_APMRESUMECRITICAL received; IsSystemResumeAutomatic=%s",
                automaticResume ? L"TRUE" : L"FALSE");
            g_agentDisplayOn = false;
            if (!automaticResume) {
                AgentHandleResume(L"PBT_APMRESUMECRITICAL user wake");
            } else {
                g_agentResumeWaitingForUserPresence = true;
            }
            return TRUE;
        }

        if (wParam == PBT_POWERSETTINGCHANGE && lParam) {
            const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);

            if (IsEqualGUID(setting->PowerSetting, kGuidSessionDisplayStatus) &&
                setting->DataLength >= sizeof(DWORD)) {
                DWORD state = *reinterpret_cast<const DWORD*>(setting->Data);
                AgentApplyDisplayState(state, L"GUID_SESSION_DISPLAY_STATUS");
                return TRUE;
            }

            if (IsEqualGUID(setting->PowerSetting, kGuidSessionUserPresence) &&
                setting->DataLength >= sizeof(DWORD)) {
                DWORD presence = *reinterpret_cast<const DWORD*>(setting->Data);
                Log(L"GUID_SESSION_USER_PRESENCE notification: %lu", presence);

                // PowerUserPresent == 0. Use this only as a resume fallback; normal
                // timeout wake continues to use LastInputInfo so presence telemetry
                // cannot cause a spontaneous re-light.
                if (presence == 0 && g_agentResumeWaitingForUserPresence) {
                    Log(L"User presence confirmed after unattended resume -> restoring panel");
                    AgentHandleResume(L"GUID_SESSION_USER_PRESENCE");
                }
                return TRUE;
            }

            if (IsEqualGUID(setting->PowerSetting, kGuidLidSwitchStateChange) &&
                setting->DataLength >= sizeof(DWORD)) {
                const DWORD lidState = *reinterpret_cast<const DWORD*>(setting->Data);
                Log(L"GUID_LIDSWITCH_STATE_CHANGE notification: %lu", lidState);
                if (lidState == 0) {
                    g_agentLidClosed = true;
                    ReleaseAgentDisplayHold(L"lid closed");
                    g_agentDisplayOn = false;
                    g_agentDimmed = false;
                    g_agentPreblanked = false;
                    HandleDisplayOff();
                } else {
                    const bool wasClosed = g_agentLidClosed;
                    g_agentLidClosed = false;
                    if (wasClosed && !g_agentWasSuspended &&
                        !g_agentResumeWaitingForUserPresence) {
                        Log(L"Lid opened while system is awake -> restoring panel");
                        g_agentDisplayOn = true;
                        HandleDisplayOn();
                    }
                }
                return TRUE;
            }

            if (IsEqualGUID(setting->PowerSetting, kGuidConsoleDisplayState) &&
                setting->DataLength >= sizeof(DWORD)) {
                DWORD state = *reinterpret_cast<const DWORD*>(setting->Data);
                if (g_agentSessionNotify) {
                    Log(L"GUID_CONSOLE_DISPLAY_STATE telemetry-only: %lu (SESSION_DISPLAY_STATUS is authoritative)", state);
                } else {
                    AgentApplyDisplayState(state, L"GUID_CONSOLE_DISPLAY_STATE fallback");
                }
                return TRUE;
            }

            if (IsEqualGUID(setting->PowerSetting, kGuidMonitorPowerOn) &&
                setting->DataLength >= sizeof(DWORD)) {
                DWORD on = *reinterpret_cast<const DWORD*>(setting->Data);
                if (g_agentSessionNotify) {
                    Log(L"GUID_MONITOR_POWER_ON telemetry-only: %lu (SESSION_DISPLAY_STATUS is authoritative)", on);
                } else {
                    AgentApplyDisplayState(on ? 1u : 0u, L"GUID_MONITOR_POWER_ON fallback");
                }
                return TRUE;
            }
        }
        return TRUE;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wParam && ((static_cast<DWORD_PTR>(lParam) & ENDSESSION_LOGOFF) == 0)) {
            Log(L"Windows shutdown/restart -> best-effort backlight OFF");
            ReleaseAgentDisplayHold(L"Windows shutdown/restart");
            HandleDisplayOff();
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kAgentTimerId);
        UnregisterHotKey(hwnd, kPanicHotkeyId);
        if (g_agentSessionNotify) {
            UnregisterPowerSettingNotification(g_agentSessionNotify);
            g_agentSessionNotify = nullptr;
        }
        if (g_agentUserPresenceNotify) {
            UnregisterPowerSettingNotification(g_agentUserPresenceNotify);
            g_agentUserPresenceNotify = nullptr;
        }
        if (g_agentConsoleNotify) {
            UnregisterPowerSettingNotification(g_agentConsoleNotify);
            g_agentConsoleNotify = nullptr;
        }
        if (g_agentLegacyNotify) {
            UnregisterPowerSettingNotification(g_agentLegacyNotify);
            g_agentLegacyNotify = nullptr;
        }
        if (g_agentLidNotify) {
            UnregisterPowerSettingNotification(g_agentLidNotify);
            g_agentLidNotify = nullptr;
        }
        if (g_agentSuspendResumeNotify) {
            UnregisterSuspendResumeNotification(g_agentSuspendResumeNotify);
            g_agentSuspendResumeNotify = nullptr;
        }
        CloseAgentDisplayPowerRequest();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int RunInteractiveAgent()
{
    HANDLE singleInstance = CreateMutexW(nullptr, FALSE,
        L"Local\\ESP32DisplayPowerAgent-SingleInstance");
    if (!singleInstance) {
        Log(L"CreateMutex(single instance) failed: %lu", GetLastError());
        return 12;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"Another ESP32 display-power agent is already running in this session; exiting duplicate");
        CloseHandle(singleInstance);
        return 0;
    }

    Log(L"Interactive display-power agent v10 failsafe starting; PID=%lu target=%s",
        GetCurrentProcessId(), kTargetPnpId);
    Log(L"Timeout sequence: HOLD T-5.5s, DIM 50%% T-5s, OFF T-2.5s, release HOLD after %lu ms",
        kFadeGuardAfterOffMs);

    // Prime the restore value and active Windows display timeout while DDC
    // is unquestionably available.
    CacheBrightnessIfAvailable();
    g_agentLastBrightnessCacheTick = GetTickCount64();
    RefreshDisplayIdlePolicy(true);
    g_agentLastPowerPlanRefreshTick = GetTickCount64();

    const wchar_t kClassName[] = L"ESP32DisplayPowerAgentHiddenWindow";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AgentWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log(L"RegisterClassExW failed: %lu", GetLastError());
        CloseHandle(singleInstance);
        return 10;
    }

    HWND hwnd = CreateWindowExW(
        0,
        kClassName,
        L"ESP32 Display Power Agent",
        WS_OVERLAPPED,
        0, 0, 0, 0,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);

    if (!hwnd) {
        Log(L"CreateWindowExW failed: %lu", GetLastError());
        CloseHandle(singleInstance);
        return 11;
    }

    // Deliberately never ShowWindow(): this is a message-only background agent
    // from the user's perspective, but a normal hidden top-level HWND ensures
    // power-setting notifications are delivered reliably.
    Log(L"Interactive agent ready; hidden HWND=%p", hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Log(L"Interactive display-power agent stopped");
    CloseHandle(singleInstance);
    return 0;
}

static void ReportServiceState(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0)
{
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwCurrentState = state;
    g_serviceStatus.dwWin32ExitCode = win32ExitCode;
    g_serviceStatus.dwWaitHint = waitHint;
    g_serviceStatus.dwControlsAccepted = 0;

    if (state == SERVICE_RUNNING) {
        g_serviceStatus.dwControlsAccepted =
            SERVICE_ACCEPT_STOP |
            SERVICE_ACCEPT_SHUTDOWN |
            SERVICE_ACCEPT_POWEREVENT;
    }

    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
}

static void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_serviceStatusHandle = RegisterServiceCtrlHandlerExW(
        kServiceName,
        ServiceControlHandler,
        nullptr
    );

    if (!g_serviceStatusHandle) {
        return;
    }

    ReportServiceState(SERVICE_START_PENDING, NO_ERROR, 5000);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_workEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!g_stopEvent || !g_workEvent) {
        ReportServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    g_powerNotify = RegisterPowerSettingNotification(
        g_serviceStatusHandle,
        &kGuidConsoleDisplayState,
        DEVICE_NOTIFY_SERVICE_HANDLE
    );

    if (!g_powerNotify) {
        DWORD err = GetLastError();
        Log(L"RegisterPowerSettingNotification failed: %lu", err);
        ReportServiceState(SERVICE_STOPPED, err);
        return;
    }

    HANDLE worker = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);
    if (!worker) {
        DWORD err = GetLastError();
        UnregisterPowerSettingNotification(g_powerNotify);
        g_powerNotify = nullptr;
        ReportServiceState(SERVICE_STOPPED, err);
        return;
    }

    Log(L"Service started; target=%s", kTargetPnpId);
    ReportServiceState(SERVICE_RUNNING);

    WaitForSingleObject(g_stopEvent, INFINITE);

    if (g_powerNotify) {
        UnregisterPowerSettingNotification(g_powerNotify);
        g_powerNotify = nullptr;
    }

    SetEvent(g_workEvent);
    WaitForSingleObject(worker, 5000);
    CloseHandle(worker);

    CloseHandle(g_workEvent);
    CloseHandle(g_stopEvent);
    g_workEvent = nullptr;
    g_stopEvent = nullptr;

    Log(L"Service stopped");
    ReportServiceState(SERVICE_STOPPED);
}

static int ManualProbe()
{
    DisplayTarget target;
    PhysicalMonitorSet set;

    wprintf(L"Target PnP ID: %s\n", kTargetPnpId);
    if (!OpenTargetPhysicalMonitors(target, set)) {
        wprintf(L"FAIL: could not resolve/open the target monitor.\n");
        wprintf(L"See: %s\n", GetLogPath().c_str());
        return 2;
    }

    wprintf(L"GDI source : %s\n", target.gdiName.c_str());
    wprintf(L"Device path: %s\n", target.monitorDevicePath.c_str());

    DWORD current = 0;
    DWORD maximum = 0;
    if (!set.GetBrightness(current, maximum)) {
        wprintf(L"FAIL: VCP 0x10 could not be read.\n");
        return 3;
    }

    wprintf(L"VCP 0x10 current=%lu max=%lu\n", current, maximum);
    wprintf(L"Saved restore brightness=%lu\n", LoadSavedBrightness());
    return 0;
}

static int ManualOff()
{
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HandleDisplayOff();
    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
    return 0;
}

static int ManualOn()
{
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HandleDisplayOn();
    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 2) {
        if (_wcsicmp(argv[1], L"--probe") == 0) {
            return ManualProbe();
        }
        if (_wcsicmp(argv[1], L"--off") == 0) {
            return ManualOff();
        }
        if (_wcsicmp(argv[1], L"--on") == 0) {
            return ManualOn();
        }
        if (_wcsicmp(argv[1], L"--agent") == 0) {
            return RunInteractiveAgent();
        }
    }

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(table)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            fwprintf(stderr,
                L"This program normally runs as the %s service.\n"
                L"Manual commands:\n"
                L"  --probe   resolve AUOD0A2 and read VCP 0x10\n"
                L"  --off     cache brightness and send VCP 0x10=0\n"
                L"  --on      restore cached brightness\n"
                L"  --agent   run hidden interactive display-power agent\n",
                kServiceName);
            return 1;
        }
        return static_cast<int>(err);
    }

    return 0;
}
