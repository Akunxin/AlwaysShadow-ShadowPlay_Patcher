#include "tray_icon.h"
#include <wchar.h>

void TrayIconInitialize(TrayIcon *tray, HWND owner, HICON icon, UINT id,
    UINT callbackMessage, const wchar_t *tooltip)
{
    ZeroMemory(tray, sizeof(*tray));
    tray->data.cbSize = sizeof(tray->data);
    tray->data.hWnd = owner;
    tray->data.uID = id;
    tray->data.uFlags = NIF_ICON | NIF_SHOWTIP | NIF_TIP | NIF_MESSAGE;
    tray->data.uCallbackMessage = callbackMessage;
    tray->data.hIcon = icon;
    wcscpy_s(tray->data.szTip, _countof(tray->data.szTip), tooltip);
}

BOOL TrayIconEnsure(TrayIcon *tray, BOOL taskbarRecreated)
{
    if (!tray->data.hWnd) return FALSE;
    if (taskbarRecreated) tray->ready = FALSE;
    if (tray->ready) return TRUE;

    // Explorer may not be ready at sign-in. MODIFY also handles an icon that
    // survived a failed attempt or a duplicate TaskbarCreated notification.
    if (!Shell_NotifyIconW(NIM_ADD, &tray->data) &&
        !Shell_NotifyIconW(NIM_MODIFY, &tray->data)) return FALSE;

    // The window procedure decodes version 4 callbacks. Do not leave an icon
    // using the legacy callback format when version negotiation fails.
    tray->data.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &tray->data)) {
        Shell_NotifyIconW(NIM_DELETE, &tray->data);
        return FALSE;
    }

    tray->ready = TRUE;
    return TRUE;
}

void TrayIconRemove(TrayIcon *tray)
{
    if (tray->data.hWnd) Shell_NotifyIconW(NIM_DELETE, &tray->data);
    // Ignore any late retry or TaskbarCreated notification during shutdown.
    ZeroMemory(tray, sizeof(*tray));
}
