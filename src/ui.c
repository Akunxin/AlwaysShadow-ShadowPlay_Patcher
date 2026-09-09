#include "ui.h"
#include "Resource.h"
#include <wchar.h>
#include <stdio.h>
#include <string.h>

static UiLanguage language = UI_ENGLISH;
static BOOL overridden = FALSE;

UiLanguage UiLanguageFor(LANGID lang) {
    return PRIMARYLANGID(lang) == LANG_CHINESE ? UI_CHINESE : UI_ENGLISH;
}

void UiInitialize(const wchar_t *override) {
    if (override && wcscmp(override, L"zh") == 0) {
        language = UI_CHINESE;
        overridden = TRUE;
    } else if (override && wcscmp(override, L"en") == 0) {
        language = UI_ENGLISH;
        overridden = TRUE;
    } else if (!overridden) {
        language = UiLanguageFor(GetUserDefaultUILanguage());
    }
}

BOOL UiIsChinese(void) { return language == UI_CHINESE; }

const wchar_t *UiText(const wchar_t *english, const wchar_t *chinese) {
    return UiIsChinese() ? chinese : english;
}

static void item(HMENU menu, UINT id, const wchar_t *english, const wchar_t *chinese, UINT flags) {
    AppendMenuW(menu, MF_STRING | flags, id, UiText(english, chinese));
}

HMENU UiCreateTrayMenu(const TrayMenuState *state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return NULL;
    if (state->preview) {
        item(menu, 0, L"Interface preview", L"界面预览", MF_GRAYED);
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    }
    if (state->disabled) {
        wchar_t text[160];
        if (state->timed)
            swprintf_s(text, _countof(text), UiText(L"Resume AlwaysShadow (paused until %02u:%02u)",
                L"恢复 AlwaysShadow（暂停至 %02u:%02u）"), state->until.wHour, state->until.wMinute);
        else
            wcscpy_s(text, _countof(text), UiText(L"Resume AlwaysShadow", L"恢复 AlwaysShadow"));
        AppendMenuW(menu, MF_STRING, ENABLE_INDEFINITE, text);
    } else {
        HMENU pause = CreatePopupMenu();
        if (!pause) { DestroyMenu(menu); return NULL; }
        item(pause, DISABLE_15MIN, L"For 15 minutes", L"暂停 15 分钟", 0);
        item(pause, DISABLE_30MIN, L"For 30 minutes", L"暂停 30 分钟", 0);
        item(pause, DISABLE_45MIN, L"For 45 minutes", L"暂停 45 分钟", 0);
        item(pause, DISABLE_1HR, L"For 1 hour", L"暂停 1 小时", 0);
        item(pause, DISABLE_2HR, L"For 2 hours", L"暂停 2 小时", 0);
        item(pause, DISABLE_3HR, L"For 3 hours", L"暂停 3 小时", 0);
        item(pause, DISABLE_4HR, L"For 4 hours", L"暂停 4 小时", 0);
        item(pause, DISABLE_CUSTOM, L"Custom duration...", L"自定义时长…", 0);
        item(pause, DISABLE_INDEFINITE, L"Until I resume", L"暂停直到手动恢复", 0);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)pause, UiText(L"Pause AlwaysShadow", L"暂停 AlwaysShadow"));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    item(menu, PROGRAM_PATCH_PROTECTION, L"Patch protection", L"补丁保护", state->patching ? MF_CHECKED : 0);
    item(menu, PROGRAM_BROWSER_PATCH, L"Browser check patch (experimental)", L"浏览器检测补丁（实验性）",
         (state->patcher.browserEnabled ? MF_CHECKED : 0) |
         (!state->patcher.browserAvailable ? MF_GRAYED : 0));
    item(menu, PROGRAM_PATCH_STATUS, L"Patch status...", L"查看补丁状态…", 0);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    item(menu, PROGRAM_REFRESH, L"Reload settings", L"重新加载设置", 0);
    item(menu, PROGRAM_OPEN_LOGS, L"Open log folder", L"打开日志文件夹", 0);
    item(menu, PROGRAM_REGISTER_STARTUP, L"Run at sign-in", L"登录时启动", state->startup ? MF_CHECKED : 0);
    if (!state->elevated && !state->preview)
        item(menu, PROGRAM_ELEVATE, L"Restart as administrator...", L"以管理员身份重新启动…", 0);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    item(menu, PROGRAM_CHECK_UPDATES_NOW, L"Check for updates now", L"立即检查更新", 0);
    item(menu, PROGRAM_CHECK_UPDATES, L"Check for updates automatically", L"自动检查更新", state->updates ? MF_CHECKED : 0);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    item(menu, PROGRAM_EXIT, L"Exit", L"退出", 0);
    return menu;
}

void UiLocalizeTimer(HWND dialog) {
    SetWindowTextW(dialog, UiText(L"Pause duration", L"暂停时长"));
    SetDlgItemTextW(dialog, HOURS_LABEL_ID, UiText(L"Hours", L"小时"));
    SetDlgItemTextW(dialog, MINUTES_LABEL_ID, UiText(L"Minutes", L"分钟"));
    SetDlgItemTextW(dialog, SECONDS_LABEL_ID, UiText(L"Seconds", L"秒"));
    SetDlgItemTextW(dialog, APPLY_PICKER_BTN_ID, UiText(L"Pause", L"暂停"));
    SetDlgItemTextW(dialog, CANCEL_PICKER_BTN_ID, UiText(L"Cancel", L"取消"));
}

static const wchar_t *stateText(PatcherState state) {
    switch (state) {
    case PATCHER_OFF: return UiText(L"Patch protection is off", L"补丁保护已关闭");
    case PATCHER_PAUSED: return UiText(L"Patch protection is paused by the current rules", L"补丁保护已按当前规则暂停");
    case PATCHER_WAITING: return UiText(L"Waiting for ShadowPlay / access to the target process", L"正在等待 ShadowPlay 或目标进程访问权限");
    case PATCHER_ACTIVE: return UiText(L"All enabled patches are applied", L"所有已启用的补丁均已生效");
    case PATCHER_PARTIAL: return UiText(L"Required patches applied; an optional patch was skipped", L"必需补丁已生效；部分可选补丁已跳过");
    case PATCHER_ERROR: return UiText(L"A patch operation failed; automatic retry remains active", L"补丁操作失败，仍会自动重试");
    case PATCHER_PREVIEW: return UiText(L"Interface preview; NVIDIA is not accessed", L"界面预览模式，未访问 NVIDIA 进程");
    }
    return L"";
}

static const wchar_t *itemText(PatcherItemState state) {
    switch (state) {
    case PATCH_ITEM_DISABLED: return UiText(L"Disabled", L"已禁用");
    case PATCH_ITEM_WAITING: return UiText(L"Waiting / not applied", L"等待中／未应用");
    case PATCH_ITEM_APPLIED: return UiText(L"Applied", L"已生效");
    case PATCH_ITEM_SKIPPED: return UiText(L"Skipped / not found / ambiguous", L"已跳过／未找到／匹配不唯一");
    case PATCH_ITEM_FAILED: return UiText(L"Failed", L"失败");
    }
    return L"";
}

void UiShowPatchStatus(HWND owner, const PatcherSnapshot *state) {
    wchar_t text[16384];
    size_t used = (size_t)swprintf_s(text, _countof(text), L"%ls\n\nPID: %lu\n",
                                    stateText(state->state), state->processId);
    for (unsigned i = 0; i < state->itemCount && used < _countof(text) - 512; ++i) {
        const PatcherItem *patch = &state->items[i];
        wchar_t name[128];
        MultiByteToWideChar(CP_UTF8, 0, patch->id, -1, name, _countof(name));
        if (strcmp(patch->id, "display_affinity") == 0)
            wcscpy_s(name, _countof(name), UiText(L"Window capture check", L"窗口防捕获检查"));
        else if (strcmp(patch->id, "module_enum") == 0)
            wcscpy_s(name, _countof(name), UiText(L"Widevine module check", L"Widevine 模块检查"));
        else if (strcmp(patch->id, "browser_detect") == 0)
            wcscpy_s(name, _countof(name), UiText(L"Browser check (experimental)", L"浏览器检测（实验性）"));
        used += (size_t)swprintf_s(text + used, _countof(text) - used, L"\n%ls: %ls", name, itemText(patch->state));
    }
    swprintf_s(text + used, _countof(text) - used, UiText(
        L"\n\nRecovery and patch protection are independent. See the log folder for details.\n\n%ls",
        L"\n\n自动恢复与补丁保护分别工作。详细信息可在日志文件夹中查看。\n\n%ls"), state->detail);
    MessageBoxW(owner, text, UiText(L"AlwaysShadow - Patch status", L"AlwaysShadow - 补丁状态"), MB_OK | MB_ICONINFORMATION);
}
