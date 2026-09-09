#ifndef ALWAYS_SHADOW_UI_H
#define ALWAYS_SHADOW_UI_H
#include <windows.h>
#include "patcher.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { UI_ENGLISH, UI_CHINESE } UiLanguage;
typedef struct {
    BOOL disabled;
    BOOL timed;
    SYSTEMTIME until;
    BOOL patching;
    BOOL startup;
    BOOL updates;
    BOOL elevated;
    BOOL preview;
    PatcherSnapshot patcher;
} TrayMenuState;

UiLanguage UiLanguageFor(LANGID language);
void UiInitialize(const wchar_t *override);
BOOL UiIsChinese(void);
const wchar_t *UiText(const wchar_t *english, const wchar_t *chinese);
HMENU UiCreateTrayMenu(const TrayMenuState *state);
void UiLocalizeTimer(HWND dialog);
void UiShowPatchStatus(HWND owner, const PatcherSnapshot *snapshot);
POINT TrayDecodePoint(WPARAM packed);
POINT TrayResolvePoint(WPARAM packed, const RECT *icon, const POINT *cursor, BOOL keyboard);
POINT TrayMenuPoint(HWND owner, UINT iconId, WPARAM packed, BOOL keyboard);

#ifdef __cplusplus
}
#endif
#endif
