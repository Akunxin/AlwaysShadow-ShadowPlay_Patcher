// NVIDIA App's ToggleIGO operation uses this versioned ShadowPlay API. See
// docs/nvidia-overlay-api.md for the ABI and the compatibility checks used here.
#include "nvidia_overlay.h"
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#define NVSP_VERSION(type) ((DWORD)(sizeof(type) | (1u << 16)))
#define NVSP_TIMEOUT_MS 5000u
// NVIDIA App owns the NvAppUI bus address (client 4). Its diagnostic client
// has an independent address, so this adapter can coexist with the open app.
#define NVSP_CLIENT_TESTING_TOOL 6u
#define NVSP_ORIGIN_USER 1u
#define NVSP_STATE_NOT_RUNNING 1u
#define NVSP_STATE_RUNNING 3u
#define NVSP_STATE_DISABLED 4u

typedef struct { DWORD version, timeout; } NvspRelease;
typedef struct { DWORD version, timeout, flags, origin; } NvspToggle;
typedef struct { DWORD version, enabled, userEnabled, state; } NvspStatus;
typedef struct NvspApi NvspApi;

// The requested v1 interface has these slots; unused slots are never called.
typedef struct
{
    HRESULT (WINAPI *release)(NvspApi *, NvspRelease *);
    FARPROC unused1;
    FARPROC unused2;
    HRESULT (WINAPI *enable)(NvspApi *, NvspToggle *);
    HRESULT (WINAPI *disable)(NvspApi *, NvspToggle *);
    HRESULT (WINAPI *getStatus)(NvspApi *, NvspStatus *);
} NvspVtable;
struct NvspApi { const NvspVtable *vtable; };
typedef struct
{
    DWORD version, interfaceVersion, client, reserved;
    NvspApi **api;
} NvspCreate;
typedef HRESULT (WINAPI *NvspCreateFn)(NvspCreate *);

_Static_assert(sizeof(void *) == 8, "The NVIDIA App overlay API requires a 64-bit build");
_Static_assert(sizeof(NvspCreate) == 24, "NVIDIA factory ABI");
_Static_assert(sizeof(NvspToggle) == 16 && sizeof(NvspStatus) == 16, "NVIDIA command ABI");

static BOOL ReadOverlayEnabled(DWORD *enabled, HRESULT *error)
{
    DWORD size = sizeof(*enabled);
    const LSTATUS result = RegGetValueW(HKEY_CURRENT_USER,
        L"SOFTWARE\\NVIDIA Corporation\\Global\\ShadowPlay\\NVSPCAPS",
        L"IsShadowPlayEnabledUser", RRF_RT_DWORD, NULL, enabled, &size);
    if (result != ERROR_SUCCESS || size != sizeof(*enabled) || *enabled > 1)
    {
        *error = HRESULT_FROM_WIN32(result == ERROR_SUCCESS ? ERROR_INVALID_DATA : result);
        return FALSE;
    }
    return TRUE;
}

static HRESULT FindOverlayApi(wchar_t *path, size_t capacity)
{
    DWORD size = (DWORD)(capacity * sizeof(*path));
    LSTATUS result = RegGetValueW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\NVIDIA Corporation\\Global\\NvApp", L"FullPath",
        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, path, &size);
    if (result == ERROR_SUCCESS)
    {
        // Derive the DLL from the machine-wide installation, including custom
        // install locations. Never search the current directory or PATH.
        const wchar_t suffix[] = L"\\CEF\\NVIDIA App.exe";
        const size_t length = wcslen(path), tail = _countof(suffix) - 1;
        if (length <= tail || path[1] != L':' || path[2] != L'\\' ||
            _wcsicmp(path + length - tail, suffix) != 0)
            return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
        path[length - tail] = L'\0';
    }
    else if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
    {
        PWSTR programFiles = NULL;
        HRESULT status = SHGetKnownFolderPath(&FOLDERID_ProgramFilesX64, 0, NULL, &programFiles);
        if (FAILED(status)) return status;
        const int written = swprintf_s(path, capacity, L"%ls\\NVIDIA Corporation\\NVIDIA App", programFiles);
        CoTaskMemFree(programFiles);
        if (written < 0) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    else return HRESULT_FROM_WIN32(result);

    if (wcscat_s(path, capacity, L"\\ShadowPlay\\nvspapi64.dll") != 0)
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    return S_OK;
}

OverlayRepairResult RepairNvidiaOverlay(OverlayRepairAllowedFn allowed, HRESULT *error)
{
    *error = S_OK;
    if (!allowed || !allowed()) return OVERLAY_REPAIR_CANCELLED;

    DWORD enabled = 0;
    if (!ReadOverlayEnabled(&enabled, error)) return OVERLAY_REPAIR_UNAVAILABLE;
    if (!enabled) return OVERLAY_REPAIR_DISABLED;

    wchar_t path[32768];
    *error = FindOverlayApi(path, _countof(path));
    if (FAILED(*error)) return OVERLAY_REPAIR_UNAVAILABLE;
    HMODULE library = LoadLibraryExW(path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library)
    {
        *error = HRESULT_FROM_WIN32(GetLastError());
        return OVERLAY_REPAIR_UNAVAILABLE;
    }

    OverlayRepairResult result = OVERLAY_REPAIR_UNAVAILABLE;
    NvspApi *api = NULL;
    union { FARPROC symbol; NvspCreateFn call; } create;
    create.symbol = GetProcAddress(library, "CreateShadowPlayApiInterface");
    if (!create.symbol)
    {
        *error = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        goto cleanup;
    }
    NvspCreate args = {NVSP_VERSION(NvspCreate), NVSP_VERSION(NvspApi), NVSP_CLIENT_TESTING_TOOL, 0, &api};
    *error = create.call(&args);
    if (FAILED(*error)) { api = NULL; goto cleanup; }
    if (!api || !api->vtable || !api->vtable->release || !api->vtable->enable ||
        !api->vtable->disable || !api->vtable->getStatus)
    {
        *error = E_NOINTERFACE;
        goto cleanup;
    }

    NvspStatus status = {NVSP_VERSION(NvspStatus), 0, 0, 0};
    *error = api->vtable->getStatus(api, &status);
    if (FAILED(*error)) goto cleanup;
    if (status.enabled > 1 || status.userEnabled > 1)
    {
        *error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        goto cleanup;
    }
    if (!status.userEnabled)
    {
        result = OVERLAY_REPAIR_DISABLED;
        goto cleanup;
    }
    if (status.state < NVSP_STATE_NOT_RUNNING || status.state > NVSP_STATE_DISABLED)
    {
        // Unknown responses do not authorize a reset. Known stopped/idle states
        // with the user's preference still ON are precisely recovery cases;
        // requiring a healthy running backend here would miss those failures.
        *error = HRESULT_FROM_WIN32(ERROR_NOT_READY);
        goto cleanup;
    }

    // NVIDIA initialization may take time. Recheck both the user preference and
    // AlwaysShadow's desktop/pause/rule guards before the first mutation.
    if (!ReadOverlayEnabled(&enabled, error)) goto cleanup;
    if (!enabled) { result = OVERLAY_REPAIR_DISABLED; goto cleanup; }
    if (!allowed()) { result = OVERLAY_REPAIR_CANCELLED; goto cleanup; }

    NvspToggle toggle = {NVSP_VERSION(NvspToggle), NVSP_TIMEOUT_MS, 0, NVSP_ORIGIN_USER};
    const HRESULT disabled = api->vtable->disable(api, &toggle);

    // A failed disable can still have partially stopped the overlay. Always
    // restore the originally enabled state, including if the user pauses or
    // exits AlwaysShadow during the native call. Retry enabling once, without
    // repeating the destructive half of the cycle.
    HRESULT restored = api->vtable->enable(api, &toggle);
    if (FAILED(restored)) restored = api->vtable->enable(api, &toggle);
    *error = FAILED(restored) ? restored : disabled;
    result = FAILED(restored) ? OVERLAY_REPAIR_RESTORE_FAILED :
        FAILED(disabled) ? OVERLAY_REPAIR_FAILED : OVERLAY_REPAIR_OK;

cleanup:
    if (api && api->vtable && api->vtable->release)
    {
        NvspRelease release = {NVSP_VERSION(NvspRelease), 1000};
        api->vtable->release(api, &release);
    }
    FreeLibrary(library);
    return result;
}
