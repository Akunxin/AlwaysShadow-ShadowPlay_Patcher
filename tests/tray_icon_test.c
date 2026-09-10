// Simulate Explorer at the Shell_NotifyIcon boundary. These tests never touch
// the real taskbar, restart Explorer or change sign-in settings.
#include "tray_icon.h"
#include <assert.h>
#include <stdio.h>
#include <wchar.h>

#define TEST_WINDOW ((HWND)(UINT_PTR)11)
#define TEST_ICON ((HICON)(UINT_PTR)12)
#define TEST_ICON_ID 17
#define TEST_CALLBACK (WM_APP + 2)

static struct
{
    BOOL available;
    BOOL exists;
    BOOL failVersion;
    BOOL failDelete;
    UINT version;
    unsigned calls[NIM_SETVERSION + 1];
} env;

static BOOL WINAPI FakeShellNotifyIcon(DWORD message, PNOTIFYICONDATAW data)
{
    assert(message <= NIM_SETVERSION);
    assert(data->cbSize == sizeof(*data));
    assert(data->hWnd == TEST_WINDOW && data->hIcon == TEST_ICON);
    assert(data->uID == TEST_ICON_ID && data->uCallbackMessage == TEST_CALLBACK);
    assert(data->uFlags == (NIF_ICON | NIF_SHOWTIP | NIF_TIP | NIF_MESSAGE));
    assert(wcscmp(data->szTip, L"AlwaysShadow - 托盘测试") == 0);
    ++env.calls[message];
    if (!env.available) return FALSE;

    switch (message) {
    case NIM_ADD:
        if (env.exists) return FALSE;
        env.exists = TRUE;
        env.version = 0;
        return TRUE;
    case NIM_MODIFY:
        return env.exists;
    case NIM_SETVERSION:
        assert(data->uVersion == NOTIFYICON_VERSION_4);
        if (!env.exists || env.failVersion) return FALSE;
        env.version = data->uVersion;
        return TRUE;
    case NIM_DELETE:
        if (!env.exists || env.failDelete) return FALSE;
        env.exists = FALSE;
        env.version = 0;
        return TRUE;
    default:
        assert(FALSE);
        return FALSE;
    }
}

#define Shell_NotifyIconW FakeShellNotifyIcon
#include "../src/tray_icon.c"

static TrayIcon NewTray(void)
{
    ZeroMemory(&env, sizeof(env));
    env.available = TRUE;
    TrayIcon tray;
    TrayIconInitialize(&tray, TEST_WINDOW, TEST_ICON, TEST_ICON_ID,
        TEST_CALLBACK, L"AlwaysShadow - 托盘测试");
    return tray;
}

static void ExpectReady(TrayIcon *tray)
{
    assert(tray->ready && env.exists);
    assert(env.version == NOTIFYICON_VERSION_4);
}

static void TestNormalStartupAndShutdown(void)
{
    TrayIcon tray = NewTray();
    assert(!tray.ready);
    assert(TrayIconEnsure(&tray, FALSE));
    ExpectReady(&tray);
    for (unsigned i = 0; i < 10; ++i) assert(TrayIconEnsure(&tray, FALSE));
    assert(env.calls[NIM_ADD] == 1 && env.calls[NIM_MODIFY] == 0);
    assert(env.calls[NIM_SETVERSION] == 1);

    TrayIconRemove(&tray);
    assert(!tray.ready && !env.exists);
    assert(!TrayIconEnsure(&tray, FALSE));
    assert(!TrayIconEnsure(&tray, TRUE));
    TrayIconRemove(&tray);
    assert(env.calls[NIM_ADD] == 1 && env.calls[NIM_DELETE] == 1);
}

static void TestSlowSignIn(void)
{
    TrayIcon tray = NewTray();
    env.available = FALSE;
    // More than two minutes of timer ticks must not exhaust retries.
    for (unsigned i = 0; i < 180; ++i) {
        assert(!TrayIconEnsure(&tray, FALSE));
        assert(!tray.ready && !env.exists);
    }
    assert(env.calls[NIM_SETVERSION] == 0);
    env.available = TRUE;
    assert(TrayIconEnsure(&tray, FALSE));
    ExpectReady(&tray);
}

static void TestVersionFailureAndRetry(void)
{
    TrayIcon tray = NewTray();
    env.failVersion = TRUE;
    assert(!TrayIconEnsure(&tray, FALSE));
    assert(!tray.ready && !env.exists);
    assert(env.calls[NIM_DELETE] == 1);

    env.failVersion = FALSE;
    assert(TrayIconEnsure(&tray, FALSE));
    ExpectReady(&tray);
    assert(env.calls[NIM_ADD] == 2);
}

static void TestRetryWithSurvivingIcon(void)
{
    TrayIcon tray = NewTray();
    env.failVersion = env.failDelete = TRUE;
    assert(!TrayIconEnsure(&tray, FALSE));
    assert(!tray.ready && env.exists);

    env.failVersion = env.failDelete = FALSE;
    assert(TrayIconEnsure(&tray, FALSE));
    ExpectReady(&tray);
    assert(env.calls[NIM_MODIFY] == 1);
}

static void TestTaskbarRecreation(void)
{
    TrayIcon tray = NewTray();
    assert(TrayIconEnsure(&tray, FALSE));
    // A duplicate broadcast must leave the existing icon usable.
    assert(TrayIconEnsure(&tray, TRUE));
    ExpectReady(&tray);
    assert(env.calls[NIM_MODIFY] == 1);

    // Explorer discarded its icons and broadcasts before it accepts new ones.
    env.exists = env.available = FALSE;
    assert(!TrayIconEnsure(&tray, TRUE));
    assert(!tray.ready);
    assert(!TrayIconEnsure(&tray, FALSE));
    env.available = TRUE;
    // Recovery must also work without another TaskbarCreated broadcast.
    assert(TrayIconEnsure(&tray, FALSE));
    ExpectReady(&tray);

    env.exists = FALSE;
    assert(TrayIconEnsure(&tray, TRUE));
    ExpectReady(&tray);
}

static void TestShutdownDuringRetry(void)
{
    TrayIcon tray = NewTray();
    env.available = FALSE;
    assert(!TrayIconEnsure(&tray, FALSE));
    TrayIconRemove(&tray);
    env.available = TRUE;
    assert(!TrayIconEnsure(&tray, FALSE));
    assert(!TrayIconEnsure(&tray, TRUE));
    assert(!env.exists && !tray.ready);
    assert(env.calls[NIM_ADD] == 1);
}

int main(void)
{
    TestNormalStartupAndShutdown();
    TestSlowSignIn();
    TestVersionFailureAndRetry();
    TestRetryWithSurvivingIcon();
    TestTaskbarRecreation();
    TestShutdownDuringRetry();
    puts("Tray startup retries, version recovery, Explorer recreation and shutdown tests passed.");
    return 0;
}
