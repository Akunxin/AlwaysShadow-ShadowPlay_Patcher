#include "ui.h"
#include <windowsx.h>
#include <shellapi.h>

POINT TrayDecodePoint(WPARAM packed) {
    // LOWORD/HIWORD alone zero-extend negative monitor coordinates to 655xx.
    POINT point = {GET_X_LPARAM((LPARAM)packed), GET_Y_LPARAM((LPARAM)packed)};
    return point;
}

POINT TrayResolvePoint(WPARAM packed, const RECT *icon, const POINT *cursor, BOOL keyboard) {
    POINT point = TrayDecodePoint(packed);
    if (keyboard || (point.x == -1 && point.y == -1)) {
        if (icon) {
            point.x = icon->left + (icon->right - icon->left) / 2;
            point.y = icon->top;
        } else if (cursor) point = *cursor;
    }
    return point;
}

POINT TrayMenuPoint(HWND owner, UINT iconId, WPARAM packed, BOOL keyboard) {
    NOTIFYICONIDENTIFIER identifier = {0};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = owner;
    identifier.uID = iconId;
    RECT icon;
    POINT cursor;
    BOOL hasIcon = SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &icon));
    BOOL hasCursor = GetCursorPos(&cursor);
    POINT point = TrayResolvePoint(packed, hasIcon ? &icon : NULL, hasCursor ? &cursor : NULL, keyboard);
    if (!MonitorFromPoint(point, MONITOR_DEFAULTTONULL)) {
        if (hasIcon) point = (POINT){icon.left, icon.top};
        else if (hasCursor) point = cursor;
    }
    return point;
}
