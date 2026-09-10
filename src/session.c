#include "session.h"
#include <wchar.h>
#include <wtsapi32.h>

static BOOL SessionUsesRemoteProtocol(DWORD sessionId)
{
    // Query the protocol on every poll: a process can move from RDP back to the
    // console without restarting. The console id alone is not a protocol check.
    LPWSTR buffer = NULL;
    DWORD bytes = 0;
    BOOL queried = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
        WTSClientProtocolType, &buffer, &bytes);
    BOOL remote = queried && buffer != NULL && bytes >= sizeof(USHORT)
        ? *(USHORT *)buffer != 0
        : GetSystemMetrics(SM_REMOTESESSION);
    if (buffer != NULL) WTSFreeMemory(buffer);
    return remote;
}

BOOL IsRemoteSession(void)
{
    DWORD sessionId;
    return ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) && SessionUsesRemoteProtocol(sessionId);
}

BOOL IsLocalInteractiveSession(void)
{
    DWORD sessionId;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) ||
        sessionId != WTSGetActiveConsoleSessionId())
    {
        // Includes RDP, disconnected sessions, fast user switching and console transitions.
        return FALSE;
    }

    // Fall back to console/desktop checks if Terminal Services is unavailable,
    // so local replay still works on machines without the service running.
    if (SessionUsesRemoteProtocol(sessionId) || GetSystemMetrics(SM_REMOTECONTROL)) return FALSE;

    // Query the actual input desktop, not the worker's desktop (which stays "Default"
    // even while Windows is locked). No keyboard input belongs on the secure desktop.
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == NULL) return FALSE;

    WCHAR name[256] = {0};
    DWORD needed;
    BOOL available = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed) &&
        _wcsicmp(name, L"Default") == 0;
    CloseDesktop(desktop);
    return available;
}
