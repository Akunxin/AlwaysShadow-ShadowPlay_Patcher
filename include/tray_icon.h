#ifndef TRAY_ICON_H
#define TRAY_ICON_H

#include <windows.h>
#include <shellapi.h>

typedef struct
{
    NOTIFYICONDATAW data;
    BOOL ready;
} TrayIcon;

void TrayIconInitialize(TrayIcon *tray, HWND owner, HICON icon, UINT id,
    UINT callbackMessage, const wchar_t *tooltip);
// Call periodically until ready, and with taskbarRecreated after TaskbarCreated.
// A failed attempt is recoverable and never waits for Explorer to start.
BOOL TrayIconEnsure(TrayIcon *tray, BOOL taskbarRecreated);
void TrayIconRemove(TrayIcon *tray);

#endif
