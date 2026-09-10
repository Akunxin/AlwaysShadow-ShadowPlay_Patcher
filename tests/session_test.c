// Exercise the real desktop guard against controlled Win32 responses, without
// locking the machine, opening a remote session or sending keyboard input.
#include <windows.h>
#include <wtsapi32.h>
#include <assert.h>
#include <stdio.h>
#include <wchar.h>

static struct
{
    DWORD processSession;
    DWORD consoleSession;
    BOOL processQueryFails;
    BOOL remoteControlled;
    BOOL remoteMetric;
    BOOL protocolQueryFails;
    BOOL shortProtocolValue;
    USHORT protocol;
    BOOL openFails;
    BOOL nameQueryFails;
    const WCHAR *desktopName;
    unsigned opened;
    unsigned closed;
    unsigned freed;
} env;

static DWORD WINAPI FakeGetCurrentProcessId(void) { return 123; }

static BOOL WINAPI FakeProcessIdToSessionId(DWORD processId, DWORD *sessionId)
{
    assert(processId == 123);
    *sessionId = env.processSession;
    return !env.processQueryFails;
}

static DWORD WINAPI FakeWTSGetActiveConsoleSessionId(void) { return env.consoleSession; }

static int WINAPI FakeGetSystemMetrics(int index)
{
    assert(index == SM_REMOTECONTROL || index == SM_REMOTESESSION);
    return index == SM_REMOTECONTROL ? env.remoteControlled : env.remoteMetric;
}

static BOOL WINAPI FakeWTSQuerySessionInformationW(HANDLE server, DWORD sessionId, WTS_INFO_CLASS info,
    LPWSTR *buffer, DWORD *bytes)
{
    assert(server == WTS_CURRENT_SERVER_HANDLE && sessionId == env.processSession && info == WTSClientProtocolType);
    if (env.protocolQueryFails) return FALSE;
    *buffer = (LPWSTR)&env.protocol;
    *bytes = env.shortProtocolValue ? 0 : sizeof(env.protocol);
    return TRUE;
}

static void WINAPI FakeWTSFreeMemory(void *buffer)
{
    assert(buffer == &env.protocol);
    env.freed++;
}

static HDESK WINAPI FakeOpenInputDesktop(DWORD flags, BOOL inherit, ACCESS_MASK access)
{
    assert(flags == 0 && !inherit && access == DESKTOP_READOBJECTS);
    if (env.openFails) return NULL;
    env.opened++;
    return (HDESK)(UINT_PTR)1234;
}

static BOOL WINAPI FakeGetUserObjectInformationW(HANDLE desktop, int index, void *buffer, DWORD length, DWORD *needed)
{
    assert(desktop == (HANDLE)(UINT_PTR)1234 && index == UOI_NAME);
    *needed = (DWORD)((wcslen(env.desktopName) + 1) * sizeof(WCHAR));
    if (env.nameQueryFails) return FALSE;
    assert(length >= *needed);
    memcpy(buffer, env.desktopName, *needed);
    return TRUE;
}

static BOOL WINAPI FakeCloseDesktop(HDESK desktop)
{
    assert(desktop == (HDESK)(UINT_PTR)1234);
    env.closed++;
    return TRUE;
}

#define GetCurrentProcessId FakeGetCurrentProcessId
#define ProcessIdToSessionId FakeProcessIdToSessionId
#define WTSGetActiveConsoleSessionId FakeWTSGetActiveConsoleSessionId
#define GetSystemMetrics FakeGetSystemMetrics
#define WTSQuerySessionInformationW FakeWTSQuerySessionInformationW
#define WTSFreeMemory FakeWTSFreeMemory
#define OpenInputDesktop FakeOpenInputDesktop
#define GetUserObjectInformationW FakeGetUserObjectInformationW
#define CloseDesktop FakeCloseDesktop
#include "../src/session.c"

static void ResetEnvironment(void)
{
    memset(&env, 0, sizeof(env));
    env.processSession = env.consoleSession = 1;
    env.desktopName = L"Default";
}

int main(void)
{
    ResetEnvironment();
    assert(IsLocalInteractiveSession());
    assert(env.opened == 1 && env.closed == 1);
    assert(env.freed == 1);

    ResetEnvironment();
    env.consoleSession = 2; // RDP, another logged-in user or a disconnected session.
    assert(!IsLocalInteractiveSession());
    assert(env.opened == 0);

    ResetEnvironment();
    env.consoleSession = (DWORD)-1; // Console is being attached/detached.
    assert(!IsLocalInteractiveSession());

    ResetEnvironment();
    env.processQueryFails = TRUE;
    assert(!IsLocalInteractiveSession());

    ResetEnvironment();
    env.remoteControlled = TRUE;
    assert(!IsLocalInteractiveSession());
    assert(env.opened == 0);

    ResetEnvironment();
    env.openFails = TRUE; // Access denied on a locked/secure desktop.
    assert(!IsLocalInteractiveSession());
    assert(env.closed == 0);

    ResetEnvironment();
    env.desktopName = L"Winlogon"; // An elevated process may be able to query it.
    assert(!IsLocalInteractiveSession());
    assert(env.opened == 1 && env.closed == 1);

    ResetEnvironment();
    env.nameQueryFails = TRUE;
    assert(!IsLocalInteractiveSession());
    assert(env.opened == 1 && env.closed == 1);

    ResetEnvironment();
    env.desktopName = L"default";
    assert(IsLocalInteractiveSession());
    assert(env.opened == 1 && env.closed == 1);

    ResetEnvironment();
    env.protocol = 2; // RDP even if the session happens to match the console id.
    assert(!IsLocalInteractiveSession());
    assert(env.freed == 1 && env.opened == 0);

    ResetEnvironment();
    env.protocolQueryFails = TRUE; // Local desktop with Terminal Services unavailable.
    assert(IsLocalInteractiveSession());
    assert(env.freed == 0);

    ResetEnvironment();
    env.protocolQueryFails = TRUE;
    env.remoteMetric = TRUE;
    assert(!IsLocalInteractiveSession());

    ResetEnvironment();
    env.shortProtocolValue = TRUE;
    env.remoteMetric = TRUE;
    assert(!IsLocalInteractiveSession());
    assert(env.freed == 1);

    ResetEnvironment();
    env.remoteMetric = TRUE; // Current WTS protocol takes precedence over the fallback.
    assert(IsLocalInteractiveSession());

    ResetEnvironment();
    env.protocol = 2;
    env.consoleSession = 2;
    assert(IsRemoteSession()); // Startup in RDP before any WTS notification.
    assert(env.opened == 0 && env.freed == 1);
    env.protocol = 0;
    assert(!IsRemoteSession()); // Requery after moving back to the console.
    assert(env.opened == 0 && env.freed == 2);

    ResetEnvironment();
    env.protocolQueryFails = TRUE;
    env.remoteMetric = TRUE;
    assert(IsRemoteSession());
    ResetEnvironment();
    env.processQueryFails = TRUE;
    assert(!IsRemoteSession());

    puts("Session tests passed (18 scenarios).");
    return 0;
}
