// AlwaysShadow - a program for forcing Shadowplay's Instant Replay to stay on.
// Copyright (C) 2024 Aviv Edery.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "Resource.h"
#include "defines.h"
#include "patcher.h"
#include "ui.h"
#include "tray_icon.h"
#include "startup.h"
#include "physical_input.h"
#include <winsock2.h>   // For libcurl, must be included before windows.h
#include <windows.h>    // For winapi.
#include <tchar.h>      // For dealing with unicode and ANSI strings.
#include <pthread.h>    // For multithreading.
#include <shlobj.h>     // For getting AppData path.
#include <direct.h>     // For making log file directory.
#include <errno.h>      // For handling mkdir errors.
#include <share.h>      // For opening a file with sharing options.
#include <time.h>       // For logging date & time.
#include <shlwapi.h>    // For dirnaming paths.
#include <curl/curl.h>  // For checking if updates exist.
#include <wtsapi32.h>   // For RDP, console connection and lock/unlock notifications.

#pragma region Declarations

#define PROGRAM_NAME TEXT("AlwaysShadow")

// The WindowClass name of the main window.
#define WC_MAINWINDOW TEXT("MainWindow")

// The UUID of the notification icon.
#define TRAY_ICON_UUID 0x69

// The ID of the timer for checking if the fixer thread has died.
#define CHECK_ALIVE_TIMER_ID 1

// The ID of the timer for enabling AlwaysShadow after a set time.
#define ENABLE_TIMER_ID 2

// The ID for a timer that allows us to flush the logs periodically.
#define FLUSH_LOGS_TIMER_ID 3

// Key + subkey for the registry path where we register to run at startup.
#define STARTUP_REGISTRY_KEY HKEY_CURRENT_USER, TEXT("Software\\Microsoft\\Windows\\CurrentVersion\\Run")
#define STARTUP_REGISTRY_VAL PROGRAM_NAME

// Key + subkey for the registry path where we store the user's setting for checking for updates.
#define UPDATES_REGISTRY_KEY HKEY_CURRENT_USER, TEXT("Software\\AlwaysShadow")
#define UPDATES_REGISTRY_VAL TEXT("DontCheckForUpdates")

// Key + subkey for the registry path where we store the squelch updates date.
#define SQUELCH_DATE_REGISTRY_KEY HKEY_CURRENT_USER, TEXT("Software\\AlwaysShadow")
#define SQUELCH_DATE_REGISTRY_VAL TEXT("SquelchDate")

#define MAKE_TIME_OPTION(t) { .amount = t, .text = TEXT(#t) }

#define MILLIS_PER_SECOND (1000u)
#define MILLIS_PER_MINUTE (60u * MILLIS_PER_SECOND)
#define MILLIS_PER_HOUR (60u * MILLIS_PER_MINUTE)

typedef struct
{
    UINT amount;
    LPTSTR text;
} TimeOption;

typedef struct
{
    HINSTANCE instanceHandle;
    HWND mainWindowHandle;
    HICON programIcon;
    HANDLE eventHandle;
    pthread_t fixerThread;
    UINT currentTimerDuration;
    SYSTEMTIME timerEndTime;
    BOOL inDialog;
    BOOL sessionNotificationsRegistered;
    BOOL workerStarted;
    BOOL preview;
    UINT taskbarCreated;
    TrayIcon trayIcon;
    BOOL trayIconRetrying;
} MainCb;

static void InitializeLogging();
static void InitializeCwd();
static void InitializeWindows(HINSTANCE instanceHandle);
static void RegisterMainWindowClass(HINSTANCE instanceHandle);
static void UninitializeWindows(HINSTANCE instanceHandle);
static char CheckOneInstance();
static LRESULT CALLBACK MainWindowProcedure(HWND windowHandle, UINT msg, WPARAM wparam, LPARAM lparam);
static LRESULT ProcessMainWindowCommand(HWND windowHandle, WPARAM wparam, LPARAM lparam);
static UINT GetMilliseconds(int id);
static SYSTEMTIME AddMillisecondsToTime(const SYSTEMTIME *sysTime, UINT millis);
static void AddNotificationIcon(BOOL taskbarRecreated);
static void ShowContextMenu(HWND hwnd, POINT pt);
static char IsStartupRegistered();
static void SetStartupRegistry(char registered);
static char IsCheckForUpdates();
static void SetCheckForUpdates(char checkForUpdates);
static char IsUpdatesSquelched();
static void SquelchUpdates();
static char IsUpdateExists(char *isUpdateExists);
void CheckForUpdates(char isManualCheck);
static void LogPatcher(const char *message);
static BOOL ReadFlag(const wchar_t *name, BOOL fallback);
static void WriteFlag(const wchar_t *name, BOOL value);
static void Panic(LPTSTR msg);
static void Warn(LPTSTR msg);
static INT_PTR TimePickerProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
static void FillListbox(HWND dialog, int id, const TimeOption *items, size_t nitems);
static int GetSelection(HWND dialog, int id);

#pragma endregion // Declarations.

#pragma region Variables

const TimeOption seconds[] =
{
    MAKE_TIME_OPTION(0), MAKE_TIME_OPTION(5), MAKE_TIME_OPTION(10), MAKE_TIME_OPTION(15),MAKE_TIME_OPTION(20), MAKE_TIME_OPTION(25),
    MAKE_TIME_OPTION(30), MAKE_TIME_OPTION(35), MAKE_TIME_OPTION(40), MAKE_TIME_OPTION(45), MAKE_TIME_OPTION(50), MAKE_TIME_OPTION(55),
};

const TimeOption minutes[] = 
{
    MAKE_TIME_OPTION(0), MAKE_TIME_OPTION(5), MAKE_TIME_OPTION(10), MAKE_TIME_OPTION(15),MAKE_TIME_OPTION(20), MAKE_TIME_OPTION(25),
    MAKE_TIME_OPTION(30), MAKE_TIME_OPTION(35), MAKE_TIME_OPTION(40), MAKE_TIME_OPTION(45), MAKE_TIME_OPTION(50), MAKE_TIME_OPTION(55),
};

const TimeOption hours[] =
{
    MAKE_TIME_OPTION(0), MAKE_TIME_OPTION(1), MAKE_TIME_OPTION(2), MAKE_TIME_OPTION(3), MAKE_TIME_OPTION(4),
    MAKE_TIME_OPTION(5), MAKE_TIME_OPTION(6), MAKE_TIME_OPTION(7), MAKE_TIME_OPTION(8), MAKE_TIME_OPTION(9),
    MAKE_TIME_OPTION(10), MAKE_TIME_OPTION(11), MAKE_TIME_OPTION(12), MAKE_TIME_OPTION(13), MAKE_TIME_OPTION(14),
    MAKE_TIME_OPTION(15), MAKE_TIME_OPTION(16), MAKE_TIME_OPTION(17), MAKE_TIME_OPTION(18), MAKE_TIME_OPTION(19),
    MAKE_TIME_OPTION(20), MAKE_TIME_OPTION(21), MAKE_TIME_OPTION(22), MAKE_TIME_OPTION(23),
};

GlobalCb glbl =
{
    .isDisabled = FALSE,
    .isRefresh = FALSE,
    .sessionChanged = FALSE,
    .fixerDied = FALSE,
    .issueWarning = FALSE,
    .errorMsg = {0},
    .warningMsg = {0},
    .lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER,

    .logfile = NULL,
    .loglock = PTHREAD_MUTEX_INITIALIZER,
};

static MainCb cb = {0};

#pragma endregion // Variables.

#pragma region Initialization

// Trying to use wWinMain causes the program to not compile. It's ok though, because we've got GetCommandLine() to get the line as unicode.
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    UiInitialize(NULL);
    int argumentCount = 0;
    LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    for (int i = 1; arguments && i < argumentCount; ++i) {
        if (wcscmp(arguments[i], L"--preview") == 0) cb.preview = TRUE;
        else if (wcsncmp(arguments[i], L"--language=", 11) == 0) UiInitialize(arguments[i] + 11);
        else if (wcscmp(arguments[i], L"--wait-for-exit") == 0 && i + 1 < argumentCount) {
            DWORD pid = wcstoul(arguments[++i], NULL, 10);
            HANDLE previous = pid != GetCurrentProcessId() ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
            if (previous) { WaitForSingleObject(previous, 15000); CloseHandle(previous); }
        }
    }
    if (arguments) LocalFree(arguments);
    typedef BOOL (WINAPI *SetDpiContextFn)(HANDLE);
    SetDpiContextFn setDpiContext = (SetDpiContextFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (setDpiContext) setDpiContext((HANDLE)-4);
    else SetProcessDPIAware();
    if (!CheckOneInstance())
    {
        PANIC(UiText(L"AlwaysShadow is already running. Open its system tray menu.",
                     L"AlwaysShadow 已在运行，请打开系统托盘菜单。"));
    }

    // The log file is a shared resource so we can't initialize it until we've ensured we're the only instance.
    InitializeLogging();
    LOG("\n\n~~~~ STARTING A RUN: BUILD DATE %s %s ~~~~\n", __DATE__, __TIME__);
    CURLcode res = curl_global_init(CURL_GLOBAL_ALL);

    if (res != CURLE_OK)
    {
        // curl_easy_strerror works even if init fails.
        // We'll just sweep this failure under the rug and let the future calls to curl also fail.
        // All the "check for updates" code is written to fail silently.
        LOG_WARN("Failed to initialize curl with error: %s", curl_easy_strerror(res));
    }

    cb.instanceHandle = hInstance;
    cb.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    glbl.patchingEnabled = cb.preview ? TRUE : ReadFlag(L"PatchProtection", TRUE);
    glbl.wakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!glbl.wakeEvent) PANIC(UiText(L"Could not create the worker event.", L"无法创建工作线程事件。"));

    // Order is important, need CWD to be set before spinning the fixer thread.
    InitializeCwd();
    PatcherInitialize(LogPatcher, cb.preview);
    InitializeWindows(hInstance);
    MSG msg = {0};

    // Entering our message loop.
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UninitializeWindows(hInstance);
    CloseHandle(glbl.wakeEvent);
    curl_global_cleanup();
    if (glbl.logfile && glbl.logfile != stderr) fclose(glbl.logfile);
    return 0;
}

static void LogPatcher(const char *message) {
    LOG("Patcher: %s", message);
}

static BOOL ReadFlag(const wchar_t *name, BOOL fallback) {
    DWORD value = fallback, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\AlwaysShadow", name,
                    RRF_RT_REG_DWORD, NULL, &value, &size) != ERROR_SUCCESS) return fallback;
    return value != 0;
}

static void WriteFlag(const wchar_t *name, BOOL value) {
    if (cb.preview) return;
    DWORD data = value != FALSE;
    LSTATUS result = RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\AlwaysShadow", name, REG_DWORD, &data, sizeof(data));
    if (result != ERROR_SUCCESS) LOG_WARN("Could not save setting %ls: %lu", name, result);
}

static void InitializeLogging()
{
    // Default to this unless assigned otherwise.
    glbl.logfile = stderr;
    if (cb.preview) {
        FILE *temporary = tmpfile();
        if (temporary) glbl.logfile = temporary;
        return;
    }

    // Get local app data path.
    wchar_t *localAppDataPath = NULL;
    HRESULT hr = SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &localAppDataPath);

    if (FAILED(hr))
    {
        LOG_ERROR("Failed to obtain LocalAppData path with error code %#lx, using stderr.", hr);
        goto exit;
    }

    // Make directory in %LOCALAPPDATA% where we'll store the logs.
    int res;
    wchar_t logfileName[1 << 13];
    swprintf_s(logfileName, _countof(logfileName), L"%ls\\AlwaysShadow", localAppDataPath);

    if ((res = _wmkdir(logfileName)) != 0 && errno != EEXIST)
    {
        LOG_ERROR("Failed to make directory for log file with error %s", strerror(errno));
        goto exit;
    }

    // Open log file in the path we've created.
    FILE *file;
    swprintf_s(logfileName, _countof(logfileName), L"%ls\\AlwaysShadow\\output.log", localAppDataPath);

    // Kinda hacky: to prevent the log file from growing forever, we have a random chance to open the file in write mode instead of append,
    // which truncates the file and behaves similar enough to append for all we care.
    srand(time(NULL));
    LPWSTR mode = rand() % 100 < 5 ? L"w" : L"a";

    // We allow reading of the log file while it is open.
    if ((file = _wfsopen(logfileName, mode, _SH_DENYWR)) == NULL)
    {
        LOG_ERROR("Failed to make log file with error %s", strerror(errno));
        goto exit;
    }

    glbl.logfile = file;

exit:
    CoTaskMemFree(localAppDataPath);
    return;
}

// Set CWD to the exe's path, where the whitelist should be located. When Windows runs programs registered at startup, it does not run them from this directory.
// There's an alternate solution that's better but cba to implement it:
// create a shortcut and set the startup path to that shortcut. When shortcuts run, they do set the CWD we want.
static void InitializeCwd()
{
    TCHAR path[MAX_PATH];
    DWORD len = GetModuleFileName(NULL, path, _countof(path));

    if (!len || len >= _countof(path))
    {
        // Fail silently.
        LOG_WARN("Failed to initialize CWD due to insufficient buffer size for path. Was able to fit: " TCS_FMT, path);
        return;
    }

    // Dirname.
    PathRemoveFileSpec(path);

    if (!SetCurrentDirectory(path))
    {
        LOG_WARN("Failed to set CWD with error: %s, path: " TCS_FMT, GetLastErrorStaticStr(), path);
        return;
    }

    LOG("Successfully set CWD to " TCS_FMT, path);
}

// Do not hold on to the returned string as it will change next time you call this function.
char *GetDateTimeStaticStr()
{
    static __thread char str[1 << 8];
    SYSTEMTIME st;
    GetLocalTime(&st);

    sprintf(str, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        st.wMilliseconds);
    return str;
}

// Do not hold on to the returned string as it will change next time you call this function.
char *GetLastErrorStaticStr()
{
    static __thread char str[1 << 8];
    DWORD err = GetLastError();
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, (LPSTR)&str, _countof(str), NULL);
    return str;
}

static void InitializeWindows(HINSTANCE instanceHandle)
{
    cb.programIcon = LoadIcon(instanceHandle, MAKEINTRESOURCE(PROGRAM_ICON_ID));
    RegisterMainWindowClass(instanceHandle);
    cb.mainWindowHandle = cb.preview
        ? CreateWindow(WC_MAINWINDOW, UiText(L"AlwaysShadow - Interface preview", L"AlwaysShadow - 界面预览"),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 520, 190, NULL, NULL, instanceHandle, NULL)
        : CreateWindow(WC_MAINWINDOW, PROGRAM_NAME, WS_MINIMIZE, 0, 0, 0, 0, 0, 0, 0, 0);
    if (cb.preview) {
        // Consume STARTUPINFO's initial show state, then display the explicitly
        // requested preview even when launched from a background build tool.
        ShowWindow(cb.mainWindowHandle, SW_SHOWDEFAULT);
        ShowWindow(cb.mainWindowHandle, SW_SHOW);
        UpdateWindow(cb.mainWindowHandle);
    }
}

static void RegisterMainWindowClass(HINSTANCE instanceHandle)
{
    char *error;
    WNDCLASS mainWindowClass = {0};
    mainWindowClass.hInstance = instanceHandle;
    mainWindowClass.lpszClassName = WC_MAINWINDOW;
    mainWindowClass.lpfnWndProc = MainWindowProcedure;
    mainWindowClass.hIcon = cb.programIcon;

    // Registering this class. If it fails, we'll log it and end the program.
    if (!RegisterClass(&mainWindowClass))
    {
        error = GetLastErrorStaticStr();
        LOG_ERROR("RegisterClass of main window failed with error: %s", error);
        PANIC(TEXT("Error initializing the program: RegisterClass error: %hs Quitting."), error);
    }
}

static void UninitializeWindows(HINSTANCE instanceHandle)
{
    UnregisterClass(WC_MAINWINDOW, instanceHandle);
}

// IMPORTANT: This function cannot use LOG because it is called before logging is initialized.
static char CheckOneInstance()
{
    cb.eventHandle = CreateEvent(NULL, FALSE, FALSE, cb.preview
        ? (UiIsChinese() ? L"Local\\AlwaysShadowPreviewZh" : L"Local\\AlwaysShadowPreviewEn")
        : L"Local\\AlwaysShadowEvent");

    if (cb.eventHandle == NULL || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(cb.eventHandle);
        cb.eventHandle = NULL;
        return FALSE;
    }

    return TRUE;
}

#pragma endregion // Initialization.

#pragma region MainWindow

static LRESULT CALLBACK MainWindowProcedure(HWND windowHandle, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (cb.taskbarCreated && msg == cb.taskbarCreated) {
        AddNotificationIcon(TRUE);
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        // Explorer normally runs unelevated, including when AlwaysShadow was
        // started by an administrator's scheduled task.
        if (cb.taskbarCreated && !ChangeWindowMessageFilterEx(windowHandle,
                cb.taskbarCreated, MSGFLT_ALLOW, NULL))
            LOG_WARN("Could not allow TaskbarCreated notifications: %s", GetLastErrorStaticStr());
        TrayIconInitialize(&cb.trayIcon, windowHandle, cb.programIcon,
            TRAY_ICON_UUID, TRAY_ICON_CALLBACK, cb.preview
                ? UiText(L"AlwaysShadow - English preview", L"AlwaysShadow - 中文预览")
                : UiText(L"AlwaysShadow - Instant Replay recovery", L"AlwaysShadow - 即时回放自动恢复"));
        if (cb.preview) {
            HWND controls[4];
            controls[0] = CreateWindowW(L"STATIC", UiText(
                L"Preview the actual tray menu and dialogs.\nNVIDIA processes and saved settings are not changed.",
                L"预览实际托盘菜单和对话框。\n此模式不会修改 NVIDIA 进程或已保存的设置。"),
                WS_CHILD | WS_VISIBLE, 20, 15, 470, 50, windowHandle, NULL, cb.instanceHandle, NULL);
            controls[1] = CreateWindowW(L"BUTTON", UiText(L"Open tray menu", L"打开托盘菜单"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 20, 85, 150, 32,
                windowHandle, (HMENU)PROGRAM_PREVIEW_MENU, cb.instanceHandle, NULL);
            controls[2] = CreateWindowW(L"BUTTON", UiText(L"Patch status", L"补丁状态"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 185, 85, 140, 32,
                windowHandle, (HMENU)PROGRAM_PATCH_STATUS, cb.instanceHandle, NULL);
            controls[3] = CreateWindowW(L"BUTTON", UiText(L"Pause duration", L"暂停时长"),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 340, 85, 140, 32,
                windowHandle, (HMENU)DISABLE_CUSTOM, cb.instanceHandle, NULL);
            for (size_t i = 0; i < _countof(controls); ++i)
                SendMessage(controls[i], WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        }
        cb.sessionNotificationsRegistered = WTSRegisterSessionNotification(windowHandle, NOTIFY_FOR_THIS_SESSION);
        if (!cb.sessionNotificationsRegistered)
            LOG_WARN("Session notifications unavailable; polling the desktop instead.");
        if (!cb.preview) {
            if (!PhysicalInputInitialize(windowHandle, glbl.wakeEvent))
                LOG_WARN("Physical input monitoring unavailable; replay recovery will wait for local hardware confirmation.");
            else
                LOG("Waiting for input from the physical PC before enabling Instant Replay recovery.");
            int result = pthread_create(&cb.fixerThread, NULL, FixerLoop, NULL);
            if (result) PANIC(UiText(L"Could not start the recovery worker (%d).", L"无法启动恢复线程（%d）。"), result);
            cb.workerStarted = TRUE;
        }
        AddNotificationIcon(FALSE);
        SetTimer(windowHandle, CHECK_ALIVE_TIMER_ID, 1000, NULL);
        SetTimer(windowHandle, FLUSH_LOGS_TIMER_ID, 60000, NULL);
        if (!cb.preview && IsCheckForUpdates() && !IsUpdatesSquelched()) CheckForUpdates(FALSE);
        return 0;
    case WM_WTSSESSION_CHANGE: {
        DWORD session;
        if (ProcessIdToSessionId(GetCurrentProcessId(), &session) && session == (DWORD)lparam) {
            LOG("Session %lu changed: %#x", session, (unsigned)wparam);
            PhysicalInputRequireConfirmation();
            pthread_mutex_lock(&glbl.lock);
            glbl.sessionChanged = TRUE;
            pthread_mutex_unlock(&glbl.lock);
            SetEvent(glbl.wakeEvent);
        }
        return 0;
    }
    case WM_INPUT:
        if (!cb.preview) PhysicalInputHandle((HRAWINPUT)lparam);
        // DefWindowProc must release foreground raw-input resources.
        return DefWindowProc(windowHandle, msg, wparam, lparam);
    case WM_INPUT_DEVICE_CHANGE:
        PhysicalInputForgetDevices();
        return 0;
    case WM_DISPLAYCHANGE:
        LOG("Display configuration changed.");
        pthread_mutex_lock(&glbl.lock);
        glbl.sessionChanged = TRUE;
        pthread_mutex_unlock(&glbl.lock);
        SetEvent(glbl.wakeEvent);
        return 0;
    case WM_SETTINGCHANGE:
        UiInitialize(NULL);
        return 0;
    case WM_COMMAND:
        return ProcessMainWindowCommand(windowHandle, wparam, lparam);
    case TRAY_ICON_CALLBACK: {
        if (!cb.trayIcon.ready) return 0;
        const UINT notification = LOWORD(lparam);
        if (notification == NIN_SELECT || notification == NIN_KEYSELECT || notification == WM_CONTEXTMENU) {
            const POINT point = TrayMenuPoint(windowHandle, TRAY_ICON_UUID, wparam, notification == NIN_KEYSELECT);
            ShowContextMenu(windowHandle, point);
        }
        return 0;
    }
    case WM_TIMER:
        if (wparam == CHECK_ALIVE_TIMER_ID) {
            if (!cb.trayIcon.ready) AddNotificationIcon(FALSE);
            pthread_mutex_lock(&glbl.lock);
            const BOOL died = glbl.fixerDied;
            const BOOL warning = glbl.issueWarning;
            pthread_mutex_unlock(&glbl.lock);
            if (died) {
                KillTimer(windowHandle, CHECK_ALIVE_TIMER_ID);
                PANIC(T_TCS_FMT, glbl.errorMsg);
            } else if (warning) {
                pthread_mutex_lock(&glbl.lock);
                glbl.issueWarning = FALSE;
                WARN(&glbl.lock, T_TCS_FMT, glbl.warningMsg);
            }
        } else if (wparam == ENABLE_TIMER_ID) {
            KillTimer(windowHandle, ENABLE_TIMER_ID);
            cb.currentTimerDuration = 0;
            pthread_mutex_lock(&glbl.lock);
            glbl.isDisabled = FALSE;
            pthread_mutex_unlock(&glbl.lock);
            SetEvent(glbl.wakeEvent);
            LOG("Pause timer expired; resuming.");
        } else if (wparam == FLUSH_LOGS_TIMER_ID) {
            pthread_mutex_lock(&glbl.loglock);
            fflush(glbl.logfile);
            pthread_mutex_unlock(&glbl.loglock);
        }
        return 0;
    case WM_ENDSESSION:
        if (wparam) SendMessage(windowHandle, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        TrayIconRemove(&cb.trayIcon);
        pthread_mutex_lock(&glbl.lock);
        glbl.isStopping = TRUE;
        pthread_mutex_unlock(&glbl.lock);
        SetEvent(glbl.wakeEvent);
        if (cb.workerStarted) {
            pthread_join(cb.fixerThread, NULL);
            cb.workerStarted = FALSE;
        } else PatcherShutdown();
        if (!cb.preview) PhysicalInputShutdown();
        CloseHandle(cb.eventHandle);
        cb.eventHandle = NULL;
        DestroyWindow(windowHandle);
        return 0;
    case WM_DESTROY:
        if (cb.sessionNotificationsRegistered) WTSUnRegisterSessionNotification(windowHandle);
        pthread_mutex_lock(&glbl.loglock);
        fflush(glbl.logfile);
        pthread_mutex_unlock(&glbl.loglock);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(windowHandle, msg, wparam, lparam);
    }
}

static LRESULT ProcessMainWindowCommand(HWND windowHandle, WPARAM wparam, LPARAM lparam)
{
    WORD wparamLow = LOWORD(wparam);

    switch (wparamLow)
    {
        case DISABLE_CUSTOM:
            cb.inDialog = TRUE;
            cb.currentTimerDuration = DialogBox(cb.instanceHandle, MAKEINTRESOURCE(TIME_PICKER_ID), windowHandle, TimePickerProc);
            cb.inDialog = FALSE;

            if (cb.currentTimerDuration > USER_TIMER_MINIMUM)
            {
                GetLocalTime(&cb.timerEndTime);
                cb.timerEndTime = AddMillisecondsToTime(&cb.timerEndTime, cb.currentTimerDuration);
                SetTimer(windowHandle, ENABLE_TIMER_ID, cb.currentTimerDuration, NULL);

                pthread_mutex_lock(&glbl.lock);
                glbl.isDisabled = TRUE;
                pthread_mutex_unlock(&glbl.lock);
                
                LOG("Disabled self for custom duration of %d millis", cb.currentTimerDuration);
            }
            else 
            {
                LOG_WARN("Not disabling self because custom duration of %d millis is below the minimum", cb.currentTimerDuration);
            }

            break;
        case DISABLE_15MIN:
        case DISABLE_30MIN:
        case DISABLE_45MIN:
        case DISABLE_1HR:
        case DISABLE_2HR:
        case DISABLE_3HR:
        case DISABLE_4HR:
        case DISABLE_INDEFINITE:
            KillTimer(windowHandle, ENABLE_TIMER_ID);
            cb.currentTimerDuration = GetMilliseconds(wparamLow);
            GetLocalTime(&cb.timerEndTime);
            cb.timerEndTime = AddMillisecondsToTime(&cb.timerEndTime, cb.currentTimerDuration);
            LOG("Received disable self request code %#x, millis = %d.", wparamLow, cb.currentTimerDuration);

            if (cb.currentTimerDuration >= USER_TIMER_MINIMUM)
            {
                SetTimer(windowHandle, ENABLE_TIMER_ID, cb.currentTimerDuration, NULL);
                LOG("Timer has been scheduled to re-enable when the time is up.");
            }

            pthread_mutex_lock(&glbl.lock);
            glbl.isDisabled = TRUE;
            pthread_mutex_unlock(&glbl.lock);
            break;
        case ENABLE_INDEFINITE:
            // If there is no timer it's no harm done.
            KillTimer(windowHandle, ENABLE_TIMER_ID);
            cb.currentTimerDuration = 0;
            LOG("Received enable request, re-enabling.");

            pthread_mutex_lock(&glbl.lock);
            glbl.isDisabled = FALSE;
            pthread_mutex_unlock(&glbl.lock);
            break;
        case PROGRAM_EXIT:
            LOG("Exit button has been pressed. Quitting.");
            PostMessage(windowHandle, WM_CLOSE, 0, 0);
            break;
        case PROGRAM_REFRESH:
            LOG("Refresh button has been pressed.");

            // Flushing the logs on refresh.
            pthread_mutex_lock(&glbl.loglock);
            fflush(glbl.logfile);
            pthread_mutex_unlock(&glbl.loglock);

            // Marking refresh for the fixer thread to detect.
            pthread_mutex_lock(&glbl.lock);
            glbl.isRefresh = TRUE;
            pthread_mutex_unlock(&glbl.lock);
            break;
        case PROGRAM_REGISTER_STARTUP:
            // Already registered, want to unregister.
            if (!cb.preview) SetStartupRegistry(!IsStartupRegistered());
            break;
        case PROGRAM_CHECK_UPDATES:
            if (!cb.preview) SetCheckForUpdates(!IsCheckForUpdates());
            break;
        case PROGRAM_CHECK_UPDATES_NOW:
            if (!cb.preview) CheckForUpdates(TRUE);
            break;
        case PROGRAM_PATCH_PROTECTION:
            pthread_mutex_lock(&glbl.lock);
            glbl.patchingEnabled = !glbl.patchingEnabled;
            WriteFlag(L"PatchProtection", glbl.patchingEnabled);
            pthread_mutex_unlock(&glbl.lock);
            break;
        case PROGRAM_BROWSER_PATCH: {
            PatcherSnapshot state;
            PatcherGetSnapshot(&state);
            if (state.browserAvailable) PatcherRequestBrowser(!state.browserEnabled);
            break;
        }
        case PROGRAM_PATCH_STATUS: {
            PatcherSnapshot state;
            PatcherGetSnapshot(&state);
            cb.inDialog = TRUE;
            UiShowPatchStatus(windowHandle, &state);
            cb.inDialog = FALSE;
            break;
        }
        case PROGRAM_PREVIEW_MENU:
            if (cb.preview)
                ShowContextMenu(windowHandle, TrayMenuPoint(windowHandle, TRAY_ICON_UUID,
                                                           MAKELPARAM(-1, -1), TRUE));
            break;
        case PROGRAM_OPEN_LOGS: {
            PWSTR local = NULL;
            if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local))) {
                wchar_t path[32768];
                swprintf_s(path, _countof(path), L"%ls\\AlwaysShadow", local);
                pthread_mutex_lock(&glbl.loglock);
                fflush(glbl.logfile);
                pthread_mutex_unlock(&glbl.loglock);
                ShellExecuteW(windowHandle, L"open", path, NULL, NULL, SW_SHOWNORMAL);
                CoTaskMemFree(local);
            }
            break;
        }
        case PROGRAM_ELEVATE: {
            if (cb.preview) break;
            wchar_t path[32768], arguments[80];
            GetModuleFileNameW(NULL, path, _countof(path));
            swprintf_s(arguments, _countof(arguments), L"--wait-for-exit %lu", GetCurrentProcessId());
            if ((INT_PTR)ShellExecuteW(windowHandle, L"runas", path, arguments, NULL, SW_SHOWNORMAL) > 32)
                PostMessage(windowHandle, WM_CLOSE, 0, 0);
            break;
        }
    }

    SetEvent(glbl.wakeEvent);
    return 0;
}

// Translates a notification code to a milliseconds duration.
static UINT GetMilliseconds(int id)
{
    switch (id)
    {
        case DISABLE_15MIN:
            return 15 * MILLIS_PER_MINUTE;
        case DISABLE_30MIN:
            return 30 * MILLIS_PER_MINUTE;
        case DISABLE_45MIN:
            return 45 * MILLIS_PER_MINUTE;
        case DISABLE_1HR:
            return 1 * MILLIS_PER_HOUR;
        case DISABLE_2HR:
            return 2 * MILLIS_PER_HOUR;
        case DISABLE_3HR:
            return 3 * MILLIS_PER_HOUR;
        case DISABLE_4HR:
            return 4 * MILLIS_PER_HOUR;
        default:
            return 0; // Important to return something less than USER_TIMER_MINIMUM in this case.
    }
}

static SYSTEMTIME AddMillisecondsToTime(const SYSTEMTIME *sysTime, UINT millis)
{
    FILETIME fileTime;
    SYSTEMTIME result;
    SystemTimeToFileTime(sysTime, &fileTime);

    ULARGE_INTEGER largeInt; 
    memcpy(&largeInt, &fileTime, sizeof(largeInt));

    const ULONGLONG millisTo100nanos = 10000;
    largeInt.QuadPart += millis * millisTo100nanos;

    memcpy(&fileTime, &largeInt, sizeof(fileTime));
    FileTimeToSystemTime(&fileTime, &result);
    return result;
}

static void AddNotificationIcon(BOOL taskbarRecreated)
{
    if (!cb.trayIcon.data.hWnd) return;
    const BOOL wasReady = cb.trayIcon.ready && !taskbarRecreated;
    if (TrayIconEnsure(&cb.trayIcon, taskbarRecreated)) {
        if (!wasReady) LOG("Notification icon successfully created.");
        cb.trayIconRetrying = FALSE;
    } else if (!cb.trayIconRetrying) {
        LOG_WARN("Notification icon unavailable; keeping AlwaysShadow running and retrying once per second.");
        cb.trayIconRetrying = TRUE;
    }
}

static void ShowContextMenu(HWND windowHandle, POINT point)
{
    if (cb.inDialog) return;
    TrayMenuState state = {0};
    pthread_mutex_lock(&glbl.lock);
    state.disabled = glbl.isDisabled;
    state.patching = glbl.patchingEnabled;
    pthread_mutex_unlock(&glbl.lock);
    state.timed = cb.currentTimerDuration != 0;
    state.until = cb.timerEndTime;
    state.preview = cb.preview;
    state.elevated = StartupIsElevated();
    state.startup = !cb.preview && IsStartupRegistered();
    state.updates = !cb.preview && IsCheckForUpdates();
    PatcherGetSnapshot(&state.patcher);
    HMENU menu = UiCreateTrayMenu(&state);
    if (!menu) return;
    SetForegroundWindow(windowHandle);
    const UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD |
        (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    const UINT command = TrackPopupMenuEx(menu, flags, point.x, point.y, windowHandle, NULL);
    PostMessage(windowHandle, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command) SendMessage(windowHandle, WM_COMMAND, command, 0);
}

static char IsStartupRegistered()
{
    if (StartupTaskExists()) return TRUE;
    LSTATUS ret = RegGetValue(STARTUP_REGISTRY_KEY, STARTUP_REGISTRY_VAL, RRF_RT_REG_SZ, NULL, NULL, NULL);

    switch (ret)
    {
        case ERROR_SUCCESS:
            return TRUE;
        case ERROR_FILE_NOT_FOUND:
            return FALSE;
        default:
            LOG_WARN("Failed to read registry key to check if program is registered at startup with error code %#lx", ret);
            return FALSE;
    }
}

static void SetStartupRegistry(char registered)
{
    if (StartupIsElevated() || StartupTaskExists()) {
        wchar_t error[512];
        if (!StartupSetTask(registered, error, _countof(error))) { Warn(error); return; }
        RegDeleteKeyValue(STARTUP_REGISTRY_KEY, STARTUP_REGISTRY_VAL);
        LOG("Updated elevated sign-in task: %d", registered);
        return;
    }
    if (registered)
    {
        TCHAR path[MAX_PATH];
        DWORD len = GetModuleFileName(NULL, path, _countof(path));

        if (len == 0 || len >= _countof(path))
        {
            LOG_WARN("Insufficient buffer size for path. Was able to fit: " TCS_FMT, path);
            WARN(NULL, TEXT("Failed to register for startup because the path to this program exceeds the maximum allowed length of %d. ")
                TEXT("Please place the program in a shorter path."), _countof(path));
            return;
        }

        HKEY hkey = NULL;
        LSTATUS ret = RegCreateKey(STARTUP_REGISTRY_KEY, &hkey);

        if (ret != ERROR_SUCCESS)
        {
            LOG_WARN("Failed to create startup registry key with result: %#lx", ret);
            WARN(NULL, TEXT("Failed to register for startup due to a problem with creating the necessary registry key. Error code: %#lx."), ret);
            return;
        }

        TCHAR quoted[MAX_PATH + 3];
        _stprintf_s(quoted, _countof(quoted), TEXT("\"%ls\""), path);
        ret = RegSetValueEx(hkey, STARTUP_REGISTRY_VAL, 0, REG_SZ, (BYTE *)quoted,
                           ((DWORD)_tcslen(quoted) + 1) * sizeof(*quoted));
        RegCloseKey(hkey);

        if (ret != ERROR_SUCCESS)
        {
            LOG_WARN("Failed to set startup registry value with result: %#lx", ret);
            WARN(NULL, TEXT("Failed to register for startup due to a problem with setting the necessary registry value. Error code: %#lx."), ret);
            return;
        }

        LOG("Successfully registered to startup");
    }
    else // Unregister.
    {
        LSTATUS ret = RegDeleteKeyValue(STARTUP_REGISTRY_KEY, STARTUP_REGISTRY_VAL);

        if (ret != ERROR_SUCCESS && ret != ERROR_FILE_NOT_FOUND)
        {
            LOG_WARN("Failed to delete startup registry key with result: %#lx", ret);
            WARN(NULL, TEXT("Failed to unregister from startup. Error code: %#lx."), ret);
            return;
        }

        LOG("Successfully unregistered from startup");
    }
}

static char IsCheckForUpdates()
{
    LSTATUS ret = RegGetValue(UPDATES_REGISTRY_KEY, UPDATES_REGISTRY_VAL, RRF_RT_REG_NONE, NULL, NULL, NULL);

    switch (ret)
    {
        // The default is that there is nothing in the registry and we check for updates. If the value is in the registry, that means *not* to check.
        case ERROR_SUCCESS:
            return FALSE;
        case ERROR_FILE_NOT_FOUND:
            return TRUE;
        default:
            LOG_WARN("Failed to read registry key to check if program should check for updates with error code %#lx", ret);
            return FALSE;
    }
}

static void SetCheckForUpdates(char checkForUpdates)
{
    if (checkForUpdates)
    {
        LSTATUS ret = RegDeleteKeyValue(UPDATES_REGISTRY_KEY, UPDATES_REGISTRY_VAL);

        if (ret != ERROR_SUCCESS)
        {
            LOG_WARN("Failed to delete updates registry key with result: %#lx", ret);
            WARN(NULL, TEXT("Failed to subscribe to updates. Error code: %#lx."), ret);
            return;
        }

        LOG("Successfully subscribed to updates");
    }
    else // Don't check for updates.
    {
        HKEY hkey = NULL;
        LSTATUS ret = RegCreateKey(UPDATES_REGISTRY_KEY, &hkey);

        if (ret != ERROR_SUCCESS)
        {
            LOG_WARN("Failed to create updates registry key with result: %#lx", ret);
            WARN(NULL, TEXT("Failed to unsubscribe to updates due to a problem with creating the necessary registry key. Error code: %#lx."), ret);
            return;
        }

        ret = RegSetValueEx(hkey, UPDATES_REGISTRY_VAL, 0, REG_NONE, NULL, 0);

        if (ret != ERROR_SUCCESS)
        {
            LOG_WARN("Failed to set updates registry value with result: %#lx", ret);
            WARN(NULL, TEXT("Failed unsubscribe to updates due to a problem with setting the necessary registry value. Error code: %#lx."), ret);
            return;
        }

        LOG("Successfully unsubscribed to updates");
    }
}

static char IsUpdatesSquelched()
{
    time_t squelchDate, now = time(NULL);
    DWORD size = sizeof(squelchDate);
    LSTATUS ret = RegGetValue(SQUELCH_DATE_REGISTRY_KEY, SQUELCH_DATE_REGISTRY_VAL, RRF_RT_REG_QWORD, NULL, &squelchDate, &size);

    switch (ret)
    {
        case ERROR_SUCCESS:
            LOG("Now: %lld, squelch date: %lld, updates are squelched: %d", now, squelchDate, now < squelchDate);
            return now < squelchDate;
        case ERROR_FILE_NOT_FOUND:
            LOG("Updates are not squelched because registry key doesn't exist");
            return FALSE;
        default:
            LOG_WARN("Failed to read registry key to check squelch date with error code %#lx", ret);
            return FALSE;
    }
}

static void SquelchUpdates()
{
    HKEY hkey = NULL;
    LSTATUS ret = RegCreateKey(SQUELCH_DATE_REGISTRY_KEY, &hkey);

    if (ret != ERROR_SUCCESS)
    {
        LOG_WARN("Failed to create squelch registry key with result: %#lx", ret);
        return;
    }

    // Squelch updates for 2 months.
    time_t squelchDate = time(NULL) + 60 * 60 * 24 * 30 * 2;
    ret = RegSetValueEx(hkey, SQUELCH_DATE_REGISTRY_VAL, 0, REG_QWORD, (BYTE *)&squelchDate, sizeof(squelchDate));

    if (ret != ERROR_SUCCESS)
    {
        LOG_WARN("Failed to set squelch registry value with result: %#lx", ret);
        return;
    }

    LOG("Successfully squelched updates until unix ts: %lld", squelchDate);
}

typedef struct
{
    char *buf;
    size_t len;
    size_t curIdx;
} AppendableBuffer;

static size_t AppendToBuffer(char *data, size_t size, size_t nmemb, void *userdata)
{
    AppendableBuffer *appendable = (AppendableBuffer *)userdata;

    // Supposed to return the amount of bytes actually taken care of. If you return less than nmemb, curl will understand it as an error.
    // If getting too much data for the buffer, return 0 to indicate the error.
    // Note we use '>=' and not '>' to make room for a null terminator.
    if (appendable->curIdx + nmemb >= appendable->len)
    {
        LOG_WARN("Buffer of size %lld starting from %lld has no room for data of size %lld", appendable->len, appendable->curIdx, nmemb);
        return 0;
    }

    // size is always 1 so nmemb is the size effectively.
    memcpy(appendable->buf + appendable->curIdx, data, nmemb);
    appendable->curIdx += nmemb;
    appendable->buf[appendable->curIdx] = '\0';
    return nmemb;
}

// Return value is success/error. Result of the check is stored in the out parameter (only if res is success).
static char IsUpdateExists(char *isUpdateExists)
{
    // Note: curl recommends reusing handles, but I don't expect this function to be called more than once so we will not do that.
    CURL *handle = curl_easy_init();
    char success = FALSE;
    *isUpdateExists = FALSE;

    if (handle == NULL)
    {
        LOG_WARN("curl_easy_init failed");
        goto cleanup;
    }

#ifdef LATEST_TAG_OVERRIDE
    char latest_tag[] = LATEST_TAG_OVERRIDE;
#else
    // This buffer only needs to be big enough for one integer really.
    char latest_tag[256] = {0};
    AppendableBuffer appendable = { .buf = latest_tag, .len = sizeof(latest_tag), .curIdx = 0 };
    long http_code;

    // Read the version.txt from GitHub, it contains the most recent version number.
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_URL, "https://raw.githubusercontent.com/" GITHUB_NAME_WITH_OWNER "/" VERSION_BRANCH_AND_FILE), "set CURLOPT_URL");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_TIMEOUT, 5), "set CURLOPT_TIMEOUT");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, AppendToBuffer), "set CURLOPT_WRITEFUNCTION");
    HANDLE_CURL_ERROR(cleanup, curl_easy_setopt(handle, CURLOPT_WRITEDATA, &appendable), "set CURLOPT_WRITEDATA");
    HANDLE_CURL_ERROR(cleanup, curl_easy_perform(handle), "read latest version number");
    HANDLE_CURL_ERROR(cleanup, curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &http_code), "get HTTP status code");
    
    if (http_code != 200) {
        LOG_WARN("Got bad HTTP status from GitHub: %ld", http_code);
        goto cleanup;
    }
#endif

    // If the downloaded latest tag is not one of the tags that were known when this version was compiled, then an update exists.
    *isUpdateExists = TRUE;
    
    for (int i = 0; i < tagsLen; i++)
    {
        if (strcmp(latest_tag, tags[i]) == 0)
        {
            *isUpdateExists = FALSE;
            LOG("Latest tag: '%s' EQUALS preexisting tag: '%s'", latest_tag, tags[i]);
            // Don't break from the loop because we want to log them all.
        }
        else
        {
            LOG("Latest tag: '%s' DOESN'T EQUAL preexisting tag: '%s'", latest_tag, tags[i]);
        }
    }

    success = TRUE;
    
cleanup:
    curl_easy_cleanup(handle); // Safe to call with NULL.
    LOG("Update exists successfully checked: %d, exists: %d", success, *isUpdateExists);
    return success;
}

void CheckForUpdates(char isManualCheck)
{
    char isUpdateExists;
    LOG("Requested to check for updates, is manual: %d", isManualCheck);

    if (!IsUpdateExists(&isUpdateExists))
    {
        // If the user checked for updates manually, give him feedback about an error with the check.
        if (isManualCheck)
        {
            MessageBox(cb.mainWindowHandle, UiText(
                L"Could not check for updates. Check your internet connection or visit the repository's releases page.",
                L"无法检查更新。请检查网络连接，或访问项目的发布页面。"),
                PROGRAM_NAME, MB_ICONINFORMATION | MB_OK);
        }

        return;
    }

    if (!isUpdateExists)
    {
        // If the user checked for updates manually, give him feedback even when there are no updates.
        if (isManualCheck)
        {
            MessageBox(cb.mainWindowHandle, UiText(L"You have the latest version.", L"当前已是最新版本。"),
                PROGRAM_NAME, MB_ICONINFORMATION | MB_OK);
        }

        return;
    }

    int choice = MessageBox(cb.mainWindowHandle, UiText(
        L"A new version of AlwaysShadow is available. Open the releases page?",
        L"发现 AlwaysShadow 新版本，是否打开发布页面？"),
        PROGRAM_NAME, MB_ICONINFORMATION | MB_YESNO);

    switch (choice)
    {
        case IDYES:
            // Opens releases page in default browser.
            ShellExecute(NULL, TEXT("open"), TEXT("https://github.com/") TEXT(GITHUB_NAME_WITH_OWNER) TEXT("/releases"), NULL, NULL, SW_SHOWNORMAL);
            LOG("User decided to get update");
            break;
        case IDNO:
            LOG("User decided not to update");
            break;
        default:
            LOG_WARN("Unknown MessageBox result: %d", choice);
            break;
    }

    // If this is an automatic check and an update was found, regardless of the user's choice about it, we want to squelch updates for a while.
    if (!isManualCheck)
    {
        SquelchUpdates();
    }
}

// IMPORTANT: This function cannot use LOG because it is called before logging is initialized.
static void Panic(LPTSTR msg)
{
    MessageBox(cb.mainWindowHandle, msg == NULL ? TEXT("An unidentified error has occured. Quitting.") : msg,
        UiText(L"AlwaysShadow - Error", L"AlwaysShadow - 错误"), MB_OK | MB_ICONERROR);
    exit(1);
}

static void Warn(LPTSTR msg)
{
    MessageBox(cb.mainWindowHandle, msg == NULL ? TEXT("An unidentified warning has warning has occured. This shouldn't happen.") : msg,
        UiText(L"AlwaysShadow - Warning", L"AlwaysShadow - 提示"), MB_OK | MB_ICONWARNING);
}

#pragma endregion // MainWindow.

#pragma region Timer Picker Dialog

static INT_PTR TimePickerProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_INITDIALOG:
            {
                UiLocalizeTimer(hDlg);
                // Add items to lists.
                FillListbox(hDlg, HOURS_LISTBOX_ID, hours, _countof(hours));
                FillListbox(hDlg, MINUTES_LISTBOX_ID, minutes, _countof(minutes));
                FillListbox(hDlg, SECONDS_LISTBOX_ID, seconds, _countof(seconds));
                SendMessage(hDlg, WM_SETICON, (WPARAM)ICON_SMALL, (LPARAM)cb.programIcon);
                return TRUE;               
            }
        case WM_CLOSE:
            EndDialog(hDlg, 0);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case APPLY_PICKER_BTN_ID:
                    {
                        int hoursSel = GetSelection(hDlg, HOURS_LISTBOX_ID);
                        int minutesSel = GetSelection(hDlg, MINUTES_LISTBOX_ID);
                        int secondsSel = GetSelection(hDlg, SECONDS_LISTBOX_ID);
                        UINT duration = hours[hoursSel].amount * MILLIS_PER_HOUR +
                                        minutes[minutesSel].amount * MILLIS_PER_MINUTE +
                                        seconds[secondsSel].amount * MILLIS_PER_SECOND;

                        EndDialog(hDlg, duration);
                        return TRUE;
                    }
                case CANCEL_PICKER_BTN_ID:
                    EndDialog(hDlg, 0);
                    return TRUE;
            }

            return TRUE;
    }

    return FALSE;
}

static void FillListbox(HWND dialog, int id, const TimeOption *items, size_t nitems)
{
    HWND listbox = GetDlgItem(dialog, id);

    for (int i = 0; i < nitems; i++)
    { 
        int pos = SendMessage(listbox, LB_ADDSTRING, 0, (LPARAM)items[i].text);

        // Set the array index of the item so we can retrieve it later.
        SendMessage(listbox, LB_SETITEMDATA, pos, (LPARAM)i); 
    }
}

static int GetSelection(HWND dialog, int id)
{
    HWND listbox = GetDlgItem(dialog, id);
    int selection = SendMessage(listbox, LB_GETCURSEL, 0, 0);

    if (selection == LB_ERR)
    {
        LOG_WARN("Got LB_ERR for %#x, error: %s (could just mean the user didn't select seconds/minutes/hours)", id, GetLastErrorStaticStr());
        selection = 0;
    }

    return selection;
}

#pragma endregion // Time Picker Dialog
