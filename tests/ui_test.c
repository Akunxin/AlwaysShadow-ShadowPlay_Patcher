#include "ui.h"
#include "Resource.h"
#include "protection_policy.h"
#include <assert.h>
#include <stdio.h>
#include <wchar.h>

static void ExpectText(HMENU menu, UINT id, const wchar_t *expected) {
    wchar_t text[256];
    assert(GetMenuStringW(menu, id, text, _countof(text), MF_BYCOMMAND) > 0);
    assert(wcscmp(text, expected) == 0);
}

int main(void) {
    assert(UiLanguageFor(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)) == UI_CHINESE);
    assert(UiLanguageFor(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)) == UI_CHINESE);
    assert(UiLanguageFor(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)) == UI_ENGLISH);
    assert(UiLanguageFor(MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT)) == UI_ENGLISH);
    TrayMenuState state = {0};
    state.patching = TRUE;
    state.patcher.browserAvailable = TRUE;
    UiInitialize(L"zh");
    HMENU menu = UiCreateTrayMenu(&state);
    assert(menu != NULL);
    ExpectText(menu, PROGRAM_PATCH_PROTECTION, L"补丁保护");
    ExpectText(menu, PROGRAM_RDP_REPLAY_FIX, L"RDP 回放 0 秒修复（重启信息浮窗）");
    assert(!(GetMenuState(menu, PROGRAM_RDP_REPLAY_FIX, MF_BYCOMMAND) & MF_CHECKED));
    ExpectText(menu, PROGRAM_EXIT, L"退出");
    ExpectText(GetSubMenu(menu, 0), DISABLE_CUSTOM, L"自定义时长…");
    assert(GetMenuState(menu, PROGRAM_PATCH_PROTECTION, MF_BYCOMMAND) & MF_CHECKED);
    DestroyMenu(menu);

    state.disabled = state.timed = TRUE;
    state.until.wHour = 9;
    state.until.wMinute = 5;
    menu = UiCreateTrayMenu(&state);
    ExpectText(menu, ENABLE_INDEFINITE, L"恢复 AlwaysShadow（暂停至 09:05）");
    DestroyMenu(menu);
    UiInitialize(L"en");
    state.rdpOverlayRecovery = TRUE;
    menu = UiCreateTrayMenu(&state);
    ExpectText(menu, ENABLE_INDEFINITE, L"Resume AlwaysShadow (paused until 09:05)");
    ExpectText(menu, PROGRAM_PATCH_STATUS, L"Patch status...");
    ExpectText(menu, PROGRAM_RDP_REPLAY_FIX, L"Fix replay after RDP (restart overlay)");
    assert(GetMenuState(menu, PROGRAM_RDP_REPLAY_FIX, MF_BYCOMMAND) & MF_CHECKED);
    DestroyMenu(menu);

    POINT point = TrayDecodePoint(MAKELPARAM(-1500, -200));
    assert(point.x == -1500 && point.y == -200);
    const RECT icon = {-1200, -60, -1176, -36};
    const POINT cursor = {300, 400};
    point = TrayResolvePoint(MAKELPARAM(-1, -1), &icon, &cursor, FALSE);
    assert(point.x == -1188 && point.y == -60);
    point = TrayResolvePoint(MAKELPARAM(0, 0), &icon, &cursor, TRUE);
    assert(point.x == -1188 && point.y == -60);
    point = TrayResolvePoint(MAKELPARAM(-1, -1), NULL, &cursor, FALSE);
    assert(point.x == 300 && point.y == 400);
    point = TrayResolvePoint(MAKELPARAM(-800, 700), &icon, &cursor, FALSE);
    assert(point.x == -800 && point.y == 700);

    assert(GetProtectionPolicy(TRUE, TRUE, FALSE, FALSE, FALSE) == PATCH_PAUSED_BY_USER);
    assert(GetProtectionPolicy(FALSE, FALSE, FALSE, FALSE, FALSE) == PATCH_WAITING_FOR_DESKTOP);
    assert(GetProtectionPolicy(FALSE, TRUE, TRUE, TRUE, TRUE) == PATCH_WHITELISTED);
    assert(GetProtectionPolicy(FALSE, TRUE, FALSE, TRUE, FALSE) == PATCH_OUTSIDE_EXCLUSIVES);
    assert(GetProtectionPolicy(FALSE, TRUE, FALSE, TRUE, TRUE) == PATCH_ALLOWED);
    assert(GetProtectionPolicy(FALSE, TRUE, FALSE, FALSE, FALSE) == PATCH_ALLOWED);
    puts("UI, signed tray coordinates, language selection and patch policy tests passed.");
    return 0;
}
