#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
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


// v10.8 predictive DIM-to-OFF policy.
//
// Windows is the sole authority for normal display power state.  The bridge:
//   * never calls PowerCreateRequest / PowerSetRequest / PowerClearRequest;
//   * never predicts from raw user-idle time;
//   * may anticipate OFF only AFTER Windows itself reports DIM;
//   * never creates a Windows power/execution-state hold.
//
// GUID_SESSION_DISPLAY_STATUS is authoritative for normal interactive-session
// display transitions:
//   PowerMonitorOn  (1) -> restore the user's saved nonzero brightness.
//   PowerMonitorDim (2) -> temporary configurable dim and optional learned pre-OFF.
//   PowerMonitorOff (0) -> authoritative VCP 0x10 = 0 and DIM->OFF learning sample.
//
// Suspend and lid-close remain independent best-effort OFF paths.
// Shutdown/restart deliberately do NOT modify VCP 0x10: Windows may still need
// the panel during shutdown/restart UI and the next boot transition.  Final
// physical power-off is left to the laptop/ESP32 hardware-side DDC guard.
static constexpr UINT_PTR kAgentTimerId = 1;
static constexpr UINT kAgentTimerPeriodMs = 100;
static constexpr ULONGLONG kBrightnessCacheIntervalMs = 1000;
static constexpr ULONGLONG kSettingsPollIntervalMs = 1000;
static constexpr ULONGLONG kRestoreRetryIntervalMs = 500;
static constexpr int kRestoreRetryMaxAttempts = 20;
static constexpr ULONGLONG kStartupDisplayStateWaitMs = 3000;
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


static std::wstring GetSettingsPath()
{
    return GetLogDirectory() + L"\\settings.ini";
}

static DWORD ClampDword(DWORD value, DWORD minimum, DWORD maximum)
{
    return (std::max)(minimum, (std::min)(value, maximum));
}

struct PredictionSettings
{
    bool enabled = true;
    bool learningEnabled = true;
    bool requireLearnedSample = true;
    bool checkDisplayRequired = true;
    DWORD dimPercent = 50;
    DWORD advanceMs = 1400;
    DWORD preOffConfirmTimeoutMs = 2500;
    DWORD fallbackDimToOffMs = 5000;
    DWORD minimumPredictionDelayMs = 500;
    DWORD minimumLearnIntervalMs = 1500;
    DWORD maximumLearnIntervalMs = 15000;
};

static PredictionSettings ReadPredictionSettingsFromIni()
{
    PredictionSettings cfg{};
    const std::wstring path = GetSettingsPath();

    cfg.dimPercent = ClampDword(
        GetPrivateProfileIntW(L"Display", L"DimPercent", 50, path.c_str()), 1, 100);

    cfg.enabled = GetPrivateProfileIntW(
        L"Prediction", L"Enabled", 1, path.c_str()) != 0;
    cfg.learningEnabled = GetPrivateProfileIntW(
        L"Prediction", L"LearningEnabled", 1, path.c_str()) != 0;
    cfg.requireLearnedSample = GetPrivateProfileIntW(
        L"Prediction", L"RequireLearnedSample", 1, path.c_str()) != 0;
    cfg.checkDisplayRequired = GetPrivateProfileIntW(
        L"Prediction", L"CheckDisplayRequired", 1, path.c_str()) != 0;
    cfg.advanceMs = ClampDword(
        GetPrivateProfileIntW(L"Prediction", L"AdvanceMs", 1400, path.c_str()), 0, 10000);
    cfg.preOffConfirmTimeoutMs = ClampDword(
        GetPrivateProfileIntW(L"Prediction", L"PreOffConfirmTimeoutMs", 2500, path.c_str()), 500, 10000);
    cfg.fallbackDimToOffMs = ClampDword(
        GetPrivateProfileIntW(L"Prediction", L"FallbackDimToOffMs", 5000, path.c_str()), 1000, 30000);
    cfg.minimumPredictionDelayMs = ClampDword(
        GetPrivateProfileIntW(L"Prediction", L"MinimumPredictionDelayMs", 500, path.c_str()), 100, 5000);
    cfg.minimumLearnIntervalMs = ClampDword(
        GetPrivateProfileIntW(L"Prediction", L"MinimumLearnIntervalMs", 1500, path.c_str()), 500, 30000);
    cfg.maximumLearnIntervalMs = ClampDword(
        GetPrivateProfileIntW(L"Prediction", L"MaximumLearnIntervalMs", 15000, path.c_str()),
        cfg.minimumLearnIntervalMs, 60000);

    return cfg;
}

static bool PredictionSettingsEqual(const PredictionSettings& a, const PredictionSettings& b)
{
    return a.enabled == b.enabled &&
        a.learningEnabled == b.learningEnabled &&
        a.requireLearnedSample == b.requireLearnedSample &&
        a.checkDisplayRequired == b.checkDisplayRequired &&
        a.dimPercent == b.dimPercent &&
        a.advanceMs == b.advanceMs &&
        a.preOffConfirmTimeoutMs == b.preOffConfirmTimeoutMs &&
        a.fallbackDimToOffMs == b.fallbackDimToOffMs &&
        a.minimumPredictionDelayMs == b.minimumPredictionDelayMs &&
        a.minimumLearnIntervalMs == b.minimumLearnIntervalMs &&
        a.maximumLearnIntervalMs == b.maximumLearnIntervalMs;
}

static DWORD LoadBridgeDword(const wchar_t* name, DWORD fallback)
{
    DWORD value = fallback;
    DWORD size = sizeof(value);
    if (RegGetValueW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\ESP32BrightnessBridge",
            name,
            RRF_RT_REG_DWORD,
            nullptr,
            &value,
            &size) != ERROR_SUCCESS) {
        return fallback;
    }
    return value;
}

static void SaveBridgeDword(const wchar_t* name, DWORD value)
{
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\ESP32BrightnessBridge",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS) {
        return;
    }

    RegSetValueExW(key, name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
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


static void SaveTemporaryDimState(bool active, DWORD value)
{
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
        &disposition);

    if (rc != ERROR_SUCCESS) {
        return;
    }

    const DWORD activeValue = active ? 1u : 0u;
    RegSetValueExW(key, L"TemporaryDimActive", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&activeValue), sizeof(activeValue));

    if (active && value > 0 && value <= 100) {
        RegSetValueExW(key, L"TemporaryDimValue", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value));
    } else {
        RegDeleteValueW(key, L"TemporaryDimValue");
    }

    RegCloseKey(key);
}

static bool LoadTemporaryDimState(DWORD& value)
{
    DWORD active = 0;
    DWORD size = sizeof(active);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\ESP32BrightnessBridge",
            L"TemporaryDimActive",
            RRF_RT_REG_DWORD,
            nullptr,
            &active,
            &size) != ERROR_SUCCESS || active == 0) {
        return false;
    }

    DWORD dimValue = 0;
    size = sizeof(dimValue);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\ESP32BrightnessBridge",
            L"TemporaryDimValue",
            RRF_RT_REG_DWORD,
            nullptr,
            &dimValue,
            &size) != ERROR_SUCCESS || dimValue == 0 || dimValue > 100) {
        return false;
    }

    value = dimValue;
    return true;
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

        // A cloned/mirrored GDI monitor can expose more than one physical
        // monitor handle. Dxva2 does not give us a stable device-path mapping
        // for those handles, so choosing the first DDC-capable one could alter
        // an external monitor instead of AUOD0A2. Fail open on ambiguity.
        if (count != 1) {
            Log(L"Refusing DDC control: GDI source %s exposes %lu physical monitors (ambiguous clone/mirror topology)",
                target.gdiName.c_str(), count);
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

    bool DetachSingle(PHYSICAL_MONITOR& out)
    {
        if (monitors_.size() != 1) {
            return false;
        }
        out = monitors_[0];
        monitors_.clear();
        return true;
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

static bool CacheBrightnessIfAvailable()
{
    DWORD current = 0;
    DWORD maximum = 0;
    if (!ReadCurrentBrightness(current, maximum)) {
        return false;
    }

    if (current > 0 && current <= 100) {
        const DWORD previous = LoadSavedBrightness();
        if (current != previous) {
            SaveBrightness(current);
            Log(L"User/external nonzero brightness adopted: %lu -> %lu", previous, current);
            return true;
        }
    }
    return false;
}

static DWORD GetTemporaryDimBrightness(DWORD dimPercent)
{
    const DWORD saved = LoadSavedBrightness();
    dimPercent = ClampDword(dimPercent, 1, 100);

    // Percentage of the user's normal brightness, rounded up so a very low
    // nonzero setting never becomes an accidental OFF command.
    DWORD dimmed = (saved * dimPercent + 99u) / 100u;
    if (dimmed == 0) {
        dimmed = 1;
    }
    return dimmed;
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

static bool HandleDisplayOn()
{
    DWORD restore = LoadSavedBrightness();
    Log(L"DISPLAY ON event -> restoring %lu%%", restore);

    // Blocking retry helper used only by the manual recovery CLI and the
    // obsolete service path.  The interactive v10.8 agent uses its own
    // non-blocking timer-driven restore state machine.
    for (int attempt = 1; attempt <= 20; ++attempt) {
        if (g_stopEvent && WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) {
            return false;
        }

        if (WriteBrightness(restore)) {
            Log(L"DISPLAY ON -> restored %lu%% (attempt %d)", restore, attempt);
            return true;
        }

        Sleep(500);
    }

    Log(L"DISPLAY ON -> restore failed after 10 seconds");
    return false;
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
 * Interactive user-session power agent -- v10.8 predictive DIM-to-OFF
 * ------------------------------------------------------------------------- */

static HPOWERNOTIFY g_agentSessionNotify = nullptr;
static HPOWERNOTIFY g_agentUserPresenceNotify = nullptr;
static HPOWERNOTIFY g_agentConsoleNotify = nullptr;
static HPOWERNOTIFY g_agentLegacyNotify = nullptr;
static HPOWERNOTIFY g_agentLidNotify = nullptr;
static HPOWERNOTIFY g_agentSuspendResumeNotify = nullptr;

static bool g_agentDisplayOn = true;
static bool g_agentDimmed = false;
static DWORD g_agentAppliedDimBrightness = 0;
static bool g_agentWasSuspended = false;
static bool g_agentResumeWaitingForUserPresence = false;
static bool g_agentLidClosed = false;
static bool g_agentSessionStateKnown = false;
static bool g_agentStartupFallbackDone = false;
// Set as soon as WM_QUERYENDSESSION arrives. While true, shutdown/restart/sign-out
// owns the UI and ALL later display-state telemetry is ignored so it cannot
// accidentally translate into VCP 0x10=0. Cleared if Windows cancels shutdown.
static bool g_agentSessionEnding = false;

static bool AgentWindowsSessionIsEnding()
{
#ifdef SM_SHUTTINGDOWN
    return g_agentSessionEnding || GetSystemMetrics(SM_SHUTTINGDOWN) != 0;
#else
    return g_agentSessionEnding;
#endif
}

static ULONGLONG g_agentStartupFallbackTick = 0;
static ULONGLONG g_agentLastBrightnessCacheTick = 0;
static ULONGLONG g_agentLastSettingsPollTick = 0;

static PredictionSettings g_agentSettings{};
static bool g_agentPredictionPending = false;
static bool g_agentPredictionPreOffSent = false;
static bool g_agentDimCycleEligibleForLearning = false;
static ULONGLONG g_agentDimStartTick = 0;
static ULONGLONG g_agentPredictionDueTick = 0;
static ULONGLONG g_agentPredictionPreOffTick = 0;

// Restore retries are deliberately non-blocking.  A slow HDMI/DDC wake must not
// block WM_POWERBROADCAST for up to ten seconds as older builds did.
static bool g_agentRestorePending = false;
static int g_agentRestoreAttempts = 0;
static ULONGLONG g_agentNextRestoreTick = 0;

// Keep one already-resolved Dxva2 physical-monitor handle while the display is
// ON. This removes DisplayConfig/HMONITOR enumeration from the critical OFF
// path. The handle is invalidated and reopened automatically if topology changes.
static HANDLE g_agentCachedPhysicalMonitor = nullptr;
static std::wstring g_agentCachedPhysicalMonitorDescription;

static void CloseAgentCachedPhysicalMonitor()
{
    if (g_agentCachedPhysicalMonitor) {
        DestroyPhysicalMonitor(g_agentCachedPhysicalMonitor);
        g_agentCachedPhysicalMonitor = nullptr;
        g_agentCachedPhysicalMonitorDescription.clear();
    }
}

static bool RefreshAgentCachedPhysicalMonitor()
{
    DisplayTarget target;
    PhysicalMonitorSet set;
    if (!OpenTargetPhysicalMonitors(target, set)) {
        return false;
    }

    PHYSICAL_MONITOR pm{};
    if (!set.DetachSingle(pm) || !pm.hPhysicalMonitor) {
        return false;
    }

    CloseAgentCachedPhysicalMonitor();
    g_agentCachedPhysicalMonitor = pm.hPhysicalMonitor;
    g_agentCachedPhysicalMonitorDescription = pm.szPhysicalMonitorDescription;
    Log(L"Cached fast DDC handle for '%s'", g_agentCachedPhysicalMonitorDescription.c_str());
    return true;
}

static bool AgentWriteBrightnessFast(DWORD value)
{
    if (g_agentCachedPhysicalMonitor) {
        SetLastError(ERROR_SUCCESS);
        if (SetVCPFeature(g_agentCachedPhysicalMonitor, kVcpBrightness, value)) {
            Log(L"FAST DDC brightness write to cached '%s': %lu",
                g_agentCachedPhysicalMonitorDescription.c_str(), value);
            return true;
        }

        Log(L"Cached DDC handle write=%lu failed (error=%lu); reopening target",
            value, GetLastError());
        CloseAgentCachedPhysicalMonitor();
    }

    if (!RefreshAgentCachedPhysicalMonitor()) {
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    if (SetVCPFeature(g_agentCachedPhysicalMonitor, kVcpBrightness, value)) {
        Log(L"FAST DDC brightness write after reopen to '%s': %lu",
            g_agentCachedPhysicalMonitorDescription.c_str(), value);
        return true;
    }

    Log(L"Reopened cached DDC handle write=%lu failed: %lu", value, GetLastError());
    CloseAgentCachedPhysicalMonitor();
    return false;
}


static void LogPredictionSettings(const PredictionSettings& cfg, const wchar_t* prefix)
{
    Log(L"%s: Enabled=%u Learning=%u RequireLearned=%u CheckDisplayRequired=%u DimPercent=%lu AdvanceMs=%lu ConfirmTimeoutMs=%lu FallbackDimToOffMs=%lu MinDelayMs=%lu LearnRange=%lu..%lu",
        prefix ? prefix : L"Settings",
        cfg.enabled ? 1u : 0u,
        cfg.learningEnabled ? 1u : 0u,
        cfg.requireLearnedSample ? 1u : 0u,
        cfg.checkDisplayRequired ? 1u : 0u,
        cfg.dimPercent,
        cfg.advanceMs,
        cfg.preOffConfirmTimeoutMs,
        cfg.fallbackDimToOffMs,
        cfg.minimumPredictionDelayMs,
        cfg.minimumLearnIntervalMs,
        cfg.maximumLearnIntervalMs);
}

static bool GetValidLearnedPredictionInterval(DWORD& interval)
{
    const DWORD learned = LoadBridgeDword(L"LearnedDimToOffMs", 0);
    const DWORD samples = LoadBridgeDword(L"LearnedDimToOffSamples", 0);
    if (samples > 0 &&
        learned >= g_agentSettings.minimumLearnIntervalMs &&
        learned <= g_agentSettings.maximumLearnIntervalMs) {
        interval = learned;
        return true;
    }
    return false;
}

static DWORD GetPredictionIntervalMs()
{
    DWORD learned = 0;
    if (g_agentSettings.learningEnabled && GetValidLearnedPredictionInterval(learned)) {
        return learned;
    }
    return g_agentSettings.fallbackDimToOffMs;
}

static void RecomputePredictionDueTick(bool logChange)
{
    if (!g_agentPredictionPending || g_agentDimStartTick == 0) {
        return;
    }

    if (!g_agentSettings.enabled) {
        g_agentPredictionPending = false;
        g_agentPredictionDueTick = 0;
        if (logChange) {
            Log(L"DIM->OFF prediction disabled by settings; waiting for authoritative Windows OFF");
        }
        return;
    }

    if (g_agentSettings.learningEnabled && g_agentSettings.requireLearnedSample) {
        DWORD learned = 0;
        if (!GetValidLearnedPredictionInterval(learned)) {
            g_agentPredictionPending = false;
            g_agentPredictionDueTick = 0;
            if (logChange) {
                Log(L"DIM->OFF calibration cycle: no valid learned sample yet; waiting for real Windows OFF");
            }
            return;
        }
    }

    const DWORD interval = GetPredictionIntervalMs();
    const DWORD rawDelay = interval > g_agentSettings.advanceMs
        ? interval - g_agentSettings.advanceMs
        : 0u;
    const DWORD delay = (std::max)(g_agentSettings.minimumPredictionDelayMs, rawDelay);
    g_agentPredictionDueTick = g_agentDimStartTick + delay;

    if (logChange) {
        Log(L"DIM->OFF prediction scheduled: interval=%lums advance=%lums -> pre-OFF at +%lums",
            interval, g_agentSettings.advanceMs, delay);
    }
}

static void ReloadPredictionSettings(bool forceLog)
{
    const PredictionSettings next = ReadPredictionSettingsFromIni();
    const bool changed = !PredictionSettingsEqual(next, g_agentSettings);
    if (changed || forceLog) {
        g_agentSettings = next;
        LogPredictionSettings(g_agentSettings, changed ? L"settings.ini reloaded" : L"settings.ini");
        if (g_agentPredictionPending) {
            RecomputePredictionDueTick(true);
        }
    }
}

static void CancelAgentPrediction(const wchar_t* reason, bool invalidateLearning)
{
    if (g_agentPredictionPending || g_agentPredictionPreOffSent) {
        Log(L"DIM->OFF prediction cancelled: %s", reason ? reason : L"no reason");
    }
    g_agentPredictionPending = false;
    g_agentPredictionDueTick = 0;
    g_agentPredictionPreOffSent = false;
    g_agentPredictionPreOffTick = 0;
    if (invalidateLearning) {
        g_agentDimCycleEligibleForLearning = false;
        g_agentDimStartTick = 0;
    }
}

static void LearnDimToOffInterval(ULONGLONG observedMs)
{
    if (!g_agentSettings.learningEnabled || !g_agentDimCycleEligibleForLearning) {
        return;
    }

    if (observedMs < g_agentSettings.minimumLearnIntervalMs ||
        observedMs > g_agentSettings.maximumLearnIntervalMs) {
        Log(L"DIM->OFF learning sample %llums ignored (allowed %lu..%lums)",
            observedMs,
            g_agentSettings.minimumLearnIntervalMs,
            g_agentSettings.maximumLearnIntervalMs);
        return;
    }

    DWORD oldAverage = LoadBridgeDword(L"LearnedDimToOffMs", 0);
    DWORD samples = LoadBridgeDword(L"LearnedDimToOffSamples", 0);
    DWORD newAverage = static_cast<DWORD>(observedMs);

    if (samples > 0 &&
        oldAverage >= g_agentSettings.minimumLearnIntervalMs &&
        oldAverage <= g_agentSettings.maximumLearnIntervalMs) {
        // Cap historical weight so the predictor can adapt if Windows changes
        // its DIM->OFF cadence after a power-policy or OS update.
        const DWORD weight = (std::min<DWORD>)(samples, 7u);
        newAverage = static_cast<DWORD>(
            (static_cast<ULONGLONG>(oldAverage) * weight + observedMs) / (weight + 1u));
    }

    const DWORD newSamples = (std::min<DWORD>)(samples + 1u, 1000000u);
    SaveBridgeDword(L"LearnedDimToOffMs", newAverage);
    SaveBridgeDword(L"LearnedDimToOffSamples", newSamples);
    Log(L"DIM->OFF learned: observed=%llums average=%lums samples=%lu",
        observedMs, newAverage, newSamples);
}

static bool QueryExternalDisplayRequired(bool& required)
{
    required = false;
    ULONG executionState = 0;
    const LONG status = static_cast<LONG>(CallNtPowerInformation(
        SystemExecutionState,
        nullptr,
        0,
        &executionState,
        sizeof(executionState)));

    if (status != 0) {
        Log(L"CallNtPowerInformation(SystemExecutionState) failed: 0x%08lX",
            static_cast<unsigned long>(status));
        return false;
    }

    required = (executionState & ES_DISPLAY_REQUIRED) != 0;
    return true;
}

static void BeginAgentDimCyclePrediction()
{
    g_agentDimStartTick = GetTickCount64();
    g_agentDimCycleEligibleForLearning = true;
    g_agentPredictionPreOffSent = false;
    g_agentPredictionPreOffTick = 0;
    g_agentPredictionPending = g_agentSettings.enabled;
    g_agentPredictionDueTick = 0;

    if (g_agentPredictionPending) {
        RecomputePredictionDueTick(true);
    } else {
        Log(L"Windows DIM accepted; prediction disabled, waiting for authoritative Windows OFF");
    }
}

static void AgentPredictionTimerTick(ULONGLONG now)
{
    if (!g_agentPredictionPending || g_agentPredictionDueTick == 0 ||
        now < g_agentPredictionDueTick) {
        return;
    }

    // Consume this prediction once. Failure always falls back to Windows' real
    // OFF notification rather than repeatedly hammering DDC.
    g_agentPredictionPending = false;
    g_agentPredictionDueTick = 0;

    if (AgentWindowsSessionIsEnding() || !g_agentDisplayOn || !g_agentDimmed ||
        g_agentResumeWaitingForUserPresence || g_agentLidClosed) {
        CancelAgentPrediction(L"state no longer eligible", true);
        return;
    }

    // If another DDC client or the ESP32 keyboard changed brightness after our
    // temporary DIM, that is user intent. Never pre-blank over it.
    DWORD current = 0;
    DWORD maximum = 0;
    if (ReadCurrentBrightness(current, maximum) && current > 0 && current <= 100 &&
        g_agentAppliedDimBrightness != 0 && current != g_agentAppliedDimBrightness) {
        SaveBrightness(current);
        Log(L"Prediction cancelled: external/user brightness changed during DIM (%lu -> %lu)",
            g_agentAppliedDimBrightness, current);
        g_agentDimmed = false;
        g_agentAppliedDimBrightness = 0;
        SaveTemporaryDimState(false, 0);
        CancelAgentPrediction(L"external brightness change", true);
        return;
    }

    if (g_agentSettings.checkDisplayRequired) {
        bool displayRequired = false;
        if (!QueryExternalDisplayRequired(displayRequired)) {
            CancelAgentPrediction(L"could not verify ES_DISPLAY_REQUIRED; fail open", false);
            return;
        }
        if (displayRequired) {
            CancelAgentPrediction(L"ES_DISPLAY_REQUIRED is active; waiting for Windows", true);
            return;
        }
    }

    if (AgentWriteBrightnessFast(0)) {
        g_agentPredictionPreOffSent = true;
        g_agentPredictionPreOffTick = now;
        Log(L"PREDICTED PRE-OFF -> VCP 0x10 = 0; waiting up to %lums for authoritative Windows OFF",
            g_agentSettings.preOffConfirmTimeoutMs);
    } else {
        Log(L"PREDICTED PRE-OFF write failed; waiting for authoritative Windows OFF (fail open)");
    }
}


static void AgentPredictionConfirmationTick(ULONGLONG now)
{
    if (!g_agentPredictionPreOffSent || g_agentPredictionPreOffTick == 0 ||
        now - g_agentPredictionPreOffTick < g_agentSettings.preOffConfirmTimeoutMs) {
        return;
    }

    // Windows did not confirm OFF soon enough. Treat the prediction as wrong
    // and recover to the temporary DIM level instead of leaving the only panel
    // black. The real Windows OFF notification remains authoritative later.
    const DWORD dimValue = g_agentAppliedDimBrightness > 0
        ? g_agentAppliedDimBrightness
        : GetTemporaryDimBrightness(g_agentSettings.dimPercent);

    Log(L"PREDICTED PRE-OFF was not confirmed within %lums -> recovering visible DIM level %lu%%",
        g_agentSettings.preOffConfirmTimeoutMs, dimValue);

    bool recovered = AgentWriteBrightnessFast(dimValue);
    if (!recovered) {
        const DWORD normal = LoadSavedBrightness();
        Log(L"DIM-level recovery failed -> trying normal saved brightness %lu%%", normal);
        recovered = AgentWriteBrightnessFast(normal);
        if (recovered) {
            g_agentDimmed = false;
            g_agentAppliedDimBrightness = 0;
            SaveTemporaryDimState(false, 0);
        }
    }

    if (!recovered) {
        Log(L"WARNING: predicted pre-OFF recovery DDC write failed; next Windows ON/emergency hotkey will retry");
    }

    CancelAgentPrediction(L"predicted OFF not confirmed", true);
}

static void CancelAgentRestore(const wchar_t* reason)
{
    if (g_agentRestorePending) {
        Log(L"Pending brightness restore cancelled: %s", reason ? reason : L"no reason");
    }
    g_agentRestorePending = false;
    g_agentRestoreAttempts = 0;
    g_agentNextRestoreTick = 0;
}

static bool AgentTryRestoreOnce()
{
    if (!g_agentRestorePending) {
        return true;
    }

    const DWORD restore = LoadSavedBrightness();
    ++g_agentRestoreAttempts;

    // If DDC is already readable and someone (ESP keyboard or another DDC app)
    // has selected a nonzero level that is not our own temporary DIM value, that
    // external choice wins. Do not overwrite it with a stale cached restore.
    DWORD current = 0;
    DWORD maximum = 0;
    if (ReadCurrentBrightness(current, maximum) && current > 0 && current <= 100 &&
        (g_agentAppliedDimBrightness == 0 || current != g_agentAppliedDimBrightness)) {
        if (current != restore) {
            SaveBrightness(current);
            Log(L"Wake restore cancelled: external/user brightness %lu%% is authoritative (cached=%lu%%)",
                current, restore);
        } else {
            Log(L"Wake restore already satisfied at %lu%%", current);
        }
        g_agentRestorePending = false;
        g_agentRestoreAttempts = 0;
        g_agentNextRestoreTick = 0;
        g_agentDimmed = false;
        g_agentAppliedDimBrightness = 0;
        SaveTemporaryDimState(false, 0);
        return true;
    }

    if (AgentWriteBrightnessFast(restore)) {
        Log(L"DISPLAY ON -> restored %lu%% (non-blocking attempt %d)",
            restore, g_agentRestoreAttempts);
        g_agentRestorePending = false;
        g_agentRestoreAttempts = 0;
        g_agentNextRestoreTick = 0;
        g_agentDimmed = false;
        g_agentAppliedDimBrightness = 0;
        SaveTemporaryDimState(false, 0);
        return true;
    }

    if (g_agentRestoreAttempts >= kRestoreRetryMaxAttempts) {
        Log(L"DISPLAY ON -> restore %lu%% failed after %d non-blocking attempts; giving up until next ON/user recovery",
            restore, kRestoreRetryMaxAttempts);
        g_agentRestorePending = false;
        g_agentNextRestoreTick = 0;
        return false;
    }

    g_agentNextRestoreTick = GetTickCount64() + kRestoreRetryIntervalMs;
    return false;
}

static void ScheduleAgentRestore(const wchar_t* reason)
{
    Log(L"DISPLAY ON -> schedule restore of saved brightness (%s)",
        reason ? reason : L"no reason");

    g_agentRestorePending = true;
    g_agentRestoreAttempts = 0;
    g_agentNextRestoreTick = GetTickCount64();

    // One immediate attempt gives power-button/keyboard wakes a fast response;
    // further attempts are timer-driven and never block the message pump.
    AgentTryRestoreOnce();
}

static void AgentTimerTick()
{
    const ULONGLONG now = GetTickCount64();

    if (g_agentLastSettingsPollTick == 0 ||
        now - g_agentLastSettingsPollTick >= kSettingsPollIntervalMs) {
        ReloadPredictionSettings(false);
        g_agentLastSettingsPollTick = now;
    }

    AgentPredictionTimerTick(now);
    AgentPredictionConfirmationTick(now);

    // RegisterPowerSettingNotification normally supplies the current session
    // display state.  If that notification is unavailable, fail toward a usable
    // screen after a short grace period rather than leaving a user permanently
    // black after an agent crash/restart.
    if (!g_agentStartupFallbackDone && !g_agentSessionStateKnown &&
        g_agentStartupFallbackTick != 0 && now >= g_agentStartupFallbackTick) {
        g_agentStartupFallbackDone = true;
        Log(L"Startup safety fallback: no SESSION_DISPLAY_STATUS received -> one fail-open brightness restore attempt");
        g_agentDisplayOn = true;
        ScheduleAgentRestore(L"startup state notification missing");
    }

    if (g_agentRestorePending && now >= g_agentNextRestoreTick) {
        AgentTryRestoreOnce();
    }

    // While Windows says the session display is ON and we are not performing a
    // temporary DIM, external/keyboard DDC changes own the brightness.  We only
    // learn them; we never write the cached value back during normal use.
    if (g_agentDisplayOn && !g_agentDimmed && !g_agentRestorePending &&
        (g_agentLastBrightnessCacheTick == 0 ||
         now - g_agentLastBrightnessCacheTick >= kBrightnessCacheIntervalMs)) {
        CacheBrightnessIfAvailable();
        g_agentLastBrightnessCacheTick = now;
    }
}

static void AgentHandleSuspend()
{
    if (AgentWindowsSessionIsEnding()) {
        Log(L"PBT_APMSUSPEND received while Windows session is ending -> brightness frozen; no VCP write");
        return;
    }

    Log(L"PBT_APMSUSPEND -> best-effort ESP32 backlight OFF; no sleep hold or delay");

    g_agentWasSuspended = true;
    g_agentResumeWaitingForUserPresence = true;
    g_agentDisplayOn = false;
    g_agentDimmed = false;
    g_agentAppliedDimBrightness = 0;
    SaveTemporaryDimState(false, 0);
    CancelAgentPrediction(L"system suspend", true);
    CancelAgentRestore(L"system suspend");

    // Do not Sleep() here.  The old one-second fade guard delayed the suspend
    // callback.  The ESP32 can continue its local fade if power remains, and its
    // DDC-bus guard is the hardware-side fallback when the laptop powers down.
    bool offOk = false;
    for (int attempt = 1; attempt <= 3 && !offOk; ++attempt) {
        offOk = AgentWriteBrightnessFast(0);
    }
    if (!offOk) {
        Log(L"Suspend OFF write failed; Windows suspend continues unimpeded");
    }
}

static void AgentHandleResume(const wchar_t* source)
{
    Log(L"%s -> user-visible resume accepted", source ? source : L"resume");

    CancelAgentPrediction(L"resume", true);
    g_agentDisplayOn = true;
    g_agentDimmed = false;
    g_agentWasSuspended = false;
    g_agentResumeWaitingForUserPresence = false;
    ScheduleAgentRestore(source ? source : L"resume");
}

static void AgentApplyDisplayState(DWORD state, const wchar_t* source)
{
    if (AgentWindowsSessionIsEnding()) {
        Log(L"%s notification %lu ignored because Windows session is ending; brightness frozen",
            source ? source : L"display", state);
        return;
    }

    const bool authoritativeSession =
        source && _wcsicmp(source, L"GUID_SESSION_DISPLAY_STATUS") == 0;
    const bool firstAuthoritativeSessionState =
        authoritativeSession && !g_agentSessionStateKnown;

    if (authoritativeSession) {
        g_agentSessionStateKnown = true;
        g_agentStartupFallbackDone = true;
    }

    Log(L"%s notification: %lu", source ? source : L"display", state);

    if (state == 0) {
        CancelAgentRestore(L"Windows display OFF");

        const bool preOffAlreadySent = g_agentPredictionPreOffSent;
        if (authoritativeSession && g_agentDimStartTick != 0 &&
            g_agentDimCycleEligibleForLearning) {
            const ULONGLONG observed = GetTickCount64() - g_agentDimStartTick;
            LearnDimToOffInterval(observed);
        }

        CancelAgentPrediction(L"authoritative Windows OFF", false);
        g_agentDimCycleEligibleForLearning = false;
        g_agentDimStartTick = 0;
        g_agentDisplayOn = false;
        g_agentDimmed = false;
        g_agentAppliedDimBrightness = 0;
        SaveTemporaryDimState(false, 0);

        // If predictive pre-OFF already succeeded, do not waste the tiny HDMI
        // teardown window repeating the same command. Otherwise this is the
        // authoritative fallback OFF path.
        if (preOffAlreadySent) {
            Log(L"Windows OFF arrived after successful predicted pre-OFF; no duplicate VCP write needed");
            return;
        }

        bool offOk = false;
        for (int attempt = 1; attempt <= 3 && !offOk; ++attempt) {
            offOk = AgentWriteBrightnessFast(0);
        }
        if (!offOk) {
            Log(L"Windows OFF -> cached/fallback DDC write failed; Windows remains authoritative and continues OFF");
        }
        return;
    }

    if (state == 1) {
        CancelAgentPrediction(L"Windows display ON", true);

        // During an unattended resume, do not light the panel merely because
        // background power telemetry changed.  Microsoft sends
        // PBT_APMRESUMESUSPEND when user activity (including the power button)
        // makes the wake interactive; GUID_SESSION_USER_PRESENCE is a fallback.
        if (g_agentResumeWaitingForUserPresence) {
            Log(L"Display ON received during resume-wait; deferring backlight until user-visible resume/presence");
            return;
        }

        // First authoritative ON after agent startup is also a crash-recovery
        // checkpoint. AgentTryRestoreOnce() adopts an already-nonzero external
        // brightness instead of overwriting it, but restores the saved value if
        // the ESP32 was left at zero by a previous crash.
        const bool needsRestore = firstAuthoritativeSessionState ||
            !g_agentDisplayOn || g_agentDimmed;
        g_agentDisplayOn = true;

        if (needsRestore) {
            ScheduleAgentRestore(authoritativeSession ?
                L"Windows SESSION display ON" : L"display ON fallback");
        } else {
            g_agentDimmed = false;
            g_agentAppliedDimBrightness = 0;
            SaveTemporaryDimState(false, 0);
        }
        return;
    }

    if (state == 2) {
        if (!g_agentDisplayOn || g_agentResumeWaitingForUserPresence || g_agentLidClosed) {
            Log(L"Windows DIM ignored because the panel is not in a normal ON state");
            return;
        }

        if (g_agentDimmed) {
            Log(L"Duplicate Windows DIM notification ignored");
            return;
        }

        // Capture a last-second external brightness change, then apply the
        // configurable temporary DIM. Prediction may begin only from this real
        // Windows DIM state; raw user-idle time is never used.
        CacheBrightnessIfAvailable();
        const DWORD appliedDim = GetTemporaryDimBrightness(g_agentSettings.dimPercent);
        if (AgentWriteBrightnessFast(appliedDim)) {
            g_agentDimmed = true;
            g_agentAppliedDimBrightness = appliedDim;
            SaveTemporaryDimState(true, appliedDim);
            Log(L"Windows DIM accepted -> temporary %lu%% of normal (%lu%%)",
                g_agentSettings.dimPercent, appliedDim);
            BeginAgentDimCyclePrediction();
        } else {
            Log(L"Windows DIM -> VCP dim failed; leaving brightness unchanged (fail open)");
        }
        return;
    }

    Log(L"Unknown display state %lu ignored", state);
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
        g_agentSuspendResumeNotify = RegisterSuspendResumeNotification(
            hwnd, DEVICE_NOTIFY_WINDOW_HANDLE);

        Log(L"Interactive registrations: session=%p presence=%p console=%p legacy=%p lid=%p suspendresume=%p",
            g_agentSessionNotify, g_agentUserPresenceNotify, g_agentConsoleNotify,
            g_agentLegacyNotify, g_agentLidNotify, g_agentSuspendResumeNotify);

        if (!g_agentSessionNotify) {
            Log(L"WARNING: Register GUID_SESSION_DISPLAY_STATUS failed: %lu; startup fail-open fallback remains active",
                GetLastError());
        }
        if (!g_agentUserPresenceNotify) {
            Log(L"WARNING: Register GUID_SESSION_USER_PRESENCE failed: %lu", GetLastError());
        }
        if (!g_agentSuspendResumeNotify) {
            Log(L"WARNING: RegisterSuspendResumeNotification failed: %lu", GetLastError());
        }

        g_agentStartupFallbackTick = GetTickCount64() + kStartupDisplayStateWaitMs;
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
            Log(L"EMERGENCY HOTKEY -> forcing saved brightness ON and cancelling transient state");
            g_agentDisplayOn = true;
            g_agentDimmed = false;
            g_agentAppliedDimBrightness = 0;
            SaveTemporaryDimState(false, 0);
            CancelAgentPrediction(L"emergency hotkey", true);
            g_agentWasSuspended = false;
            g_agentResumeWaitingForUserPresence = false;
            ScheduleAgentRestore(L"emergency hotkey");
            return 0;
        }
        break;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND) {
            AgentHandleSuspend();
            return TRUE;
        }

        if (wParam == PBT_APMRESUMEAUTOMATIC) {
            Log(L"PBT_APMRESUMEAUTOMATIC -> waiting for user-visible resume/presence");
            g_agentDisplayOn = false;
            g_agentResumeWaitingForUserPresence = true;
            return TRUE;
        }

        if (wParam == PBT_APMRESUMESUSPEND) {
            AgentHandleResume(L"PBT_APMRESUMESUSPEND");
            return TRUE;
        }

        if (wParam == PBT_APMRESUMECRITICAL) {
            // Critical resume means the normal suspend notification sequence was
            // incomplete.  Fail toward a usable screen rather than risking black.
            AgentHandleResume(L"PBT_APMRESUMECRITICAL");
            return TRUE;
        }

        if (wParam == PBT_POWERSETTINGCHANGE && lParam) {
            const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);

            if (IsEqualGUID(setting->PowerSetting, kGuidSessionDisplayStatus) &&
                setting->DataLength >= sizeof(DWORD)) {
                const DWORD state = *reinterpret_cast<const DWORD*>(setting->Data);
                AgentApplyDisplayState(state, L"GUID_SESSION_DISPLAY_STATUS");
                return TRUE;
            }

            if (IsEqualGUID(setting->PowerSetting, kGuidSessionUserPresence) &&
                setting->DataLength >= sizeof(DWORD)) {
                const DWORD presence = *reinterpret_cast<const DWORD*>(setting->Data);
                Log(L"GUID_SESSION_USER_PRESENCE notification: %lu", presence);

                // PowerUserPresent == 0.
                if (presence == 0 && g_agentResumeWaitingForUserPresence) {
                    AgentHandleResume(L"GUID_SESSION_USER_PRESENCE");
                }
                return TRUE;
            }

            if (IsEqualGUID(setting->PowerSetting, kGuidLidSwitchStateChange) &&
                setting->DataLength >= sizeof(DWORD)) {
                const DWORD lidState = *reinterpret_cast<const DWORD*>(setting->Data);
                Log(L"GUID_LIDSWITCH_STATE_CHANGE notification: %lu", lidState);

                if (lidState == 0) {
                    if (AgentWindowsSessionIsEnding()) {
                        Log(L"Lid-close notification during shutdown/restart -> brightness frozen; no VCP write");
                        return TRUE;
                    }
                    g_agentLidClosed = true;
                    CancelAgentRestore(L"lid closed");
                    g_agentDisplayOn = false;
                    g_agentDimmed = false;
                    g_agentAppliedDimBrightness = 0;
                    SaveTemporaryDimState(false, 0);
                    CancelAgentPrediction(L"lid closed", true);
                    AgentWriteBrightnessFast(0);
                } else {
                    g_agentLidClosed = false;
                    // Do not restore solely because the lid opened.  Windows'
                    // SESSION display ON notification decides whether the panel
                    // should actually light.
                    Log(L"Lid opened -> waiting for Windows SESSION display ON");
                }
                return TRUE;
            }

            if (IsEqualGUID(setting->PowerSetting, kGuidConsoleDisplayState) &&
                setting->DataLength >= sizeof(DWORD)) {
                const DWORD state = *reinterpret_cast<const DWORD*>(setting->Data);
                if (g_agentSessionNotify) {
                    Log(L"GUID_CONSOLE_DISPLAY_STATE telemetry-only: %lu", state);
                } else {
                    AgentApplyDisplayState(state, L"GUID_CONSOLE_DISPLAY_STATE fallback");
                }
                return TRUE;
            }

            if (IsEqualGUID(setting->PowerSetting, kGuidMonitorPowerOn) &&
                setting->DataLength >= sizeof(DWORD)) {
                const DWORD on = *reinterpret_cast<const DWORD*>(setting->Data);
                if (g_agentSessionNotify) {
                    Log(L"GUID_MONITOR_POWER_ON telemetry-only: %lu", on);
                } else {
                    AgentApplyDisplayState(on ? 1u : 0u, L"GUID_MONITOR_POWER_ON fallback");
                }
                return TRUE;
            }
        }
        return TRUE;

    case WM_QUERYENDSESSION:
        // Freeze brightness BEFORE Windows begins sending shutdown/restart
        // display telemetry. In particular, ignore a SESSION_DISPLAY_STATUS=OFF
        // that can arrive during shutdown; that OFF is not a normal idle/display
        // timeout and must not be translated into VCP 0x10=0.
        g_agentSessionEnding = true;
        CancelAgentPrediction(L"Windows session ending", true);
        CancelAgentRestore(L"Windows session ending");
        Log(L"WM_QUERYENDSESSION -> freezing panel brightness; shutdown/restart display notifications will be ignored");
        return TRUE;

    case WM_ENDSESSION:
        if (wParam) {
            // Do not alter panel brightness for shutdown, restart, or sign-out.
            // Windows may still render shutdown/restart UI, and a restart should
            // carry the user's current brightness cleanly into the next boot.
            // The ESP32/laptop hardware-side DDC guard handles final power loss.
            Log(L"Windows session ending -> panel brightness left unchanged");
        } else {
            // Shutdown/restart was cancelled; resume normal display telemetry.
            g_agentSessionEnding = false;
            Log(L"WM_ENDSESSION cancelled -> brightness freeze released");
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kAgentTimerId);
        UnregisterHotKey(hwnd, kPanicHotkeyId);
        CancelAgentPrediction(L"agent shutdown", true);
        CancelAgentRestore(L"agent shutdown");

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
        CloseAgentCachedPhysicalMonitor();
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

    Log(L"Interactive display-power agent v10.8 predictive DIM-to-OFF starting; PID=%lu target=%s",
        GetCurrentProcessId(), kTargetPnpId);
    Log(L"Power policy: NO PowerRequest/ExecutionState holds; Windows SESSION DIM is required before any prediction");
    ReloadPredictionSettings(true);
    g_agentLastSettingsPollTick = GetTickCount64();
    Log(L"Display sequence: Windows DIM -> configurable dim + learned pre-OFF; Windows OFF remains authoritative fallback; Windows ON -> non-blocking restore");

    DWORD persistedDim = 0;
    if (LoadTemporaryDimState(persistedDim)) {
        g_agentDimmed = true;
        g_agentAppliedDimBrightness = persistedDim;
        Log(L"Crash recovery marker found: temporary DIM %lu%% was active", persistedDim);
    }

    // Learn the current user's brightness if DDC is already available. If a
    // persisted temporary-DIM marker exists, do not adopt that exact transient
    // value as the user's normal brightness.
    DWORD startupCurrent = 0;
    DWORD startupMaximum = 0;
    if (ReadCurrentBrightness(startupCurrent, startupMaximum) &&
        startupCurrent > 0 && startupCurrent <= 100) {
        if (!g_agentDimmed || startupCurrent != g_agentAppliedDimBrightness) {
            SaveBrightness(startupCurrent);
            Log(L"Startup brightness adopted: %lu%%", startupCurrent);
        } else {
            Log(L"Startup brightness %lu%% matches persisted temporary DIM; keeping saved normal brightness", startupCurrent);
        }
    }
    g_agentLastBrightnessCacheTick = GetTickCount64();
    RefreshAgentCachedPhysicalMonitor();

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

static int ManualSyncBrightness()
{
    DWORD current = 0;
    DWORD maximum = 0;
    if (!ReadCurrentBrightness(current, maximum)) {
        wprintf(L"FAIL: VCP 0x10 could not be read.\n");
        return 2;
    }

    if (current == 0 || current > 100) {
        wprintf(L"Brightness is %lu; not adopting zero/invalid value as the normal restore level.\n", current);
        return 3;
    }

    SaveBrightness(current);
    wprintf(L"Saved current user brightness: %lu\n", current);
    Log(L"Manual sync adopted current brightness=%lu", current);
    return 0;
}

static int ManualOff()
{
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool ok = HandleDisplayOff();
    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
    return ok ? 0 : 2;
}

static int ManualOn()
{
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool ok = HandleDisplayOn();
    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
    return ok ? 0 : 2;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 2) {
        if (_wcsicmp(argv[1], L"--probe") == 0) {
            return ManualProbe();
        }
        if (_wcsicmp(argv[1], L"--sync") == 0) {
            return ManualSyncBrightness();
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
                L"  --sync    adopt current nonzero VCP 0x10 as restore brightness\n"
                L"  --off     send VCP 0x10=0\n"
                L"  --on      restore cached brightness\n"
                L"  --agent   run hidden interactive display-power agent\n",
                kServiceName);
            return 1;
        }
        return static_cast<int>(err);
    }

    return 0;
}
