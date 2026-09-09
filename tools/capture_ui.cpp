// Documentation renderer: capture this process's own native menus/dialogs.
// Uses the same UI functions/resources as AlwaysShadow. It never opens NVIDIA.
#include "ui.h"
#include "Resource.h"
#include <gdiplus.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::wstring outputPath;
std::wstring mode;
bool captured = false;
constexpr UINT_PTR captureTimer = 11;
constexpr UINT iconId = 77;
HWND owner = nullptr;

bool saveWindow(HWND window) {
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return false;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    // Capture the actual native popup, including non-client frame; no UI mockup.
    if (!PrintWindow(window, memory, 2))
        BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    SelectObject(memory, previous);
    bool saved = false;
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        UINT count = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&count, &size);
        std::vector<BYTE> encoders(size);
        auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(encoders.data());
        Gdiplus::GetImageEncoders(count, size, codecs);
        for (UINT i = 0; i < count; ++i) {
            if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
                saved = image.Save(outputPath.c_str(), &codecs[i].Clsid, nullptr) == Gdiplus::Ok;
                break;
            }
        }
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (saved) wprintf(L"%ls: %d x %d, screen (%ld, %ld)\n", outputPath.c_str(), width, height, rect.left, rect.top);
    return saved;
}

BOOL CALLBACK findPopup(HWND window, LPARAM result) {
    wchar_t className[80]{};
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(window)) return TRUE;
    GetClassNameW(window, className, _countof(className));
    const bool wanted = mode == L"menu" || mode == L"paused"
        ? wcscmp(className, L"#32768") == 0 : wcscmp(className, L"#32770") == 0;
    if (!wanted) return TRUE;
    *reinterpret_cast<HWND*>(result) = window;
    return FALSE;
}

INT_PTR CALLBACK timerDialog(HWND dialog, UINT message, WPARAM wparam, LPARAM) {
    if (message == WM_INITDIALOG) {
        UiLocalizeTimer(dialog);
        for (int column = 0; column < 3; ++column) {
            const int id = column == 0 ? HOURS_LISTBOX_ID : column == 1 ? MINUTES_LISTBOX_ID : SECONDS_LISTBOX_ID;
            const int count = column == 0 ? 24 : 12;
            for (int i = 0; i < count; ++i) {
                wchar_t value[16];
                swprintf_s(value, L"%d", column == 0 ? i : i * 5);
                SendDlgItemMessageW(dialog, id, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
            }
        }
        SetTimer(dialog, captureTimer, 700, nullptr);
        return TRUE;
    }
    if (message == WM_TIMER && wparam == captureTimer) {
        KillTimer(dialog, captureTimer);
        captured = saveWindow(dialog);
        EndDialog(dialog, 0);
        return TRUE;
    }
    return FALSE;
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_APP + 1) {
        if (mode == L"timer") {
            DialogBoxW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(TIME_PICKER_ID), window, timerDialog);
        } else {
            SetTimer(window, captureTimer, 700, nullptr);
            if (mode == L"status") {
                PatcherSnapshot state{};
                state.state = PATCHER_PREVIEW;
                state.itemCount = 3;
                const char* names[] = {"display_affinity", "module_enum", "browser_detect"};
                for (unsigned i = 0; i < 3; ++i) {
                    strcpy_s(state.items[i].id, names[i]);
                    state.items[i].state = i < 2 ? PATCH_ITEM_WAITING : PATCH_ITEM_DISABLED;
                }
                UiShowPatchStatus(window, &state);
            } else {
                TrayMenuState state{};
                state.patching = state.elevated = TRUE;
                state.patcher.browserAvailable = TRUE;
                state.disabled = state.timed = mode == L"paused";
                state.until.wHour = 18;
                state.until.wMinute = 30;
                HMENU menu = UiCreateTrayMenu(&state);
                const POINT point = TrayMenuPoint(window, iconId, MAKELPARAM(-1, -1), TRUE);
                SetForegroundWindow(window);
                TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, window, nullptr);
                DestroyMenu(menu);
            }
        }
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_TIMER && wparam == captureTimer) {
        KillTimer(window, captureTimer);
        HWND popup = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), findPopup, reinterpret_cast<LPARAM>(&popup));
        if (popup) captured = saveWindow(popup);
        if (mode == L"menu" || mode == L"paused") EndMenu();
        else if (popup) PostMessageW(popup, WM_CLOSE, 0, 0);
        return 0;
    }
    if (message == WM_DESTROY) {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window;
        icon.uID = iconId;
        Shell_NotifyIconW(NIM_DELETE, &icon);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 || (wcscmp(argv[1], L"en") != 0 && wcscmp(argv[1], L"zh") != 0) ||
        (wcscmp(argv[2], L"menu") != 0 && wcscmp(argv[2], L"paused") != 0 &&
         wcscmp(argv[2], L"timer") != 0 && wcscmp(argv[2], L"status") != 0)) {
        fwprintf(stderr, L"Usage: capture_ui.exe en|zh menu|paused|timer|status output.png\n");
        return 2;
    }
    UiInitialize(argv[1]);
    mode = argv[2];
    outputPath = argv[3];
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return 1;
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpfnWndProc = windowProcedure;
    type.lpszClassName = L"AlwaysShadowDocumentationCapture";
    RegisterClassW(&type);
    owner = CreateWindowW(type.lpszClassName, L"AlwaysShadow documentation capture",
                           WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, type.hInstance, nullptr);
    if (!owner) { Gdiplus::GdiplusShutdown(token); return 1; }
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = owner;
    icon.uID = iconId;
    icon.uFlags = NIF_ICON | NIF_TIP;
    icon.hIcon = LoadIconW(type.hInstance, MAKEINTRESOURCEW(PROGRAM_ICON_ID));
    wcscpy_s(icon.szTip, L"AlwaysShadow - Documentation");
    Shell_NotifyIconW(NIM_ADD, &icon);
    PostMessageW(owner, WM_APP + 1, 0, 0);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Gdiplus::GdiplusShutdown(token);
    return captured ? 0 : 1;
}
