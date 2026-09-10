// Exercise the real native API adapter against fake NVIDIA/Win32 boundaries.
// No NVIDIA DLL is loaded and no user settings or recording state are changed.
#include "nvidia_overlay.h"
#include <assert.h>
#include <shlobj.h>
#include <stdio.h>
#include <wchar.h>

static LSTATUS WINAPI FakeRegGetValueW(HKEY, LPCWSTR, LPCWSTR, DWORD, DWORD *, void *, DWORD *);
static HMODULE WINAPI FakeLoadLibraryExW(LPCWSTR, HANDLE, DWORD);
static FARPROC WINAPI FakeGetProcAddress(HMODULE, LPCSTR);
static BOOL WINAPI FakeFreeLibrary(HMODULE);
static HRESULT WINAPI FakeSHGetKnownFolderPath(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR *);

#define RegGetValueW FakeRegGetValueW
#define LoadLibraryExW FakeLoadLibraryExW
#define GetProcAddress FakeGetProcAddress
#define FreeLibrary FakeFreeLibrary
#define SHGetKnownFolderPath FakeSHGetKnownFolderPath
#include "../src/nvidia_overlay.c"

static struct
{
    DWORD preference, status, runtimeEnabled, userEnabled;
    LSTATUS preferenceError, installError;
    BOOL shortPreference, missingLibrary, missingExport, malformedInterface;
    BOOL allowed, cancelAfterStatus, disableAfterStatus, cancelDuringDisable;
    HRESULT createError, queryError, disableError;
    unsigned enableFailures;
    unsigned loads, frees, creates, releases, queries, disables, enables, permissionChecks;
    const wchar_t *installPath;
} env;

static const HMODULE fakeLibrary = (HMODULE)(UINT_PTR)123;

static LSTATUS WINAPI FakeRegGetValueW(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD flags,
    DWORD *type, void *data, DWORD *size)
{
    assert(type == NULL);
    if (key == HKEY_CURRENT_USER)
    {
        assert(wcscmp(subkey, L"SOFTWARE\\NVIDIA Corporation\\Global\\ShadowPlay\\NVSPCAPS") == 0);
        assert(wcscmp(value, L"IsShadowPlayEnabledUser") == 0);
        assert(flags == RRF_RT_DWORD && *size == sizeof(DWORD)); // DWORD or four-byte binary.
        *(DWORD *)data = env.preference;
        *size = env.shortPreference ? 1 : sizeof(DWORD);
        return env.preferenceError;
    }
    assert(key == HKEY_LOCAL_MACHINE);
    assert(wcscmp(subkey, L"SOFTWARE\\NVIDIA Corporation\\Global\\NvApp") == 0);
    assert(wcscmp(value, L"FullPath") == 0 && flags == (RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY));
    if (env.installError != ERROR_SUCCESS) return env.installError;
    const DWORD required = (DWORD)((wcslen(env.installPath) + 1) * sizeof(wchar_t));
    assert(*size >= required);
    memcpy(data, env.installPath, required);
    *size = required;
    return ERROR_SUCCESS;
}

static HRESULT WINAPI FakeSHGetKnownFolderPath(REFKNOWNFOLDERID folder, DWORD flags, HANDLE token, PWSTR *out)
{
    assert(IsEqualGUID(folder, &FOLDERID_ProgramFilesX64) && flags == 0 && token == NULL);
    const wchar_t path[] = L"D:\\Program Files";
    *out = CoTaskMemAlloc(sizeof(path));
    assert(*out);
    memcpy(*out, path, sizeof(path));
    return S_OK;
}

static HMODULE WINAPI FakeLoadLibraryExW(LPCWSTR path, HANDLE file, DWORD flags)
{
    assert(file == NULL && flags == (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
    assert(wcscmp(path, env.installError == ERROR_FILE_NOT_FOUND
        ? L"D:\\Program Files\\NVIDIA Corporation\\NVIDIA App\\ShadowPlay\\nvspapi64.dll"
        : L"C:\\Custom NVIDIA\\ShadowPlay\\nvspapi64.dll") == 0);
    ++env.loads;
    if (env.missingLibrary) { SetLastError(ERROR_MOD_NOT_FOUND); return NULL; }
    return fakeLibrary;
}

static HRESULT WINAPI FakeRelease(NvspApi *api, NvspRelease *args)
{
    assert(api && args->version == 0x10008 && args->timeout == 1000);
    ++env.releases;
    return S_OK;
}

static HRESULT WINAPI FakeStatus(NvspApi *api, NvspStatus *args)
{
    assert(api && args->version == 0x10010 && args->enabled == 0 && args->userEnabled == 0);
    ++env.queries;
    args->state = env.status;
    args->enabled = env.runtimeEnabled;
    args->userEnabled = env.userEnabled;
    if (env.cancelAfterStatus) env.allowed = FALSE;
    if (env.disableAfterStatus) env.preference = 0;
    return env.queryError;
}

static void ExpectToggle(const NvspToggle *args)
{
    assert(args->version == 0x10010 && args->timeout == NVSP_TIMEOUT_MS);
    assert(args->flags == 0 && args->origin == 1);
}

static HRESULT WINAPI FakeDisable(NvspApi *api, NvspToggle *args)
{
    assert(api && env.preference == 1 && env.allowed);
    ExpectToggle(args);
    ++env.disables;
    env.preference = 0; // Even an error may have partially stopped the overlay.
    if (env.cancelDuringDisable) env.allowed = FALSE;
    return env.disableError;
}

static HRESULT WINAPI FakeEnable(NvspApi *api, NvspToggle *args)
{
    assert(api && env.disables == 1);
    ExpectToggle(args);
    ++env.enables;
    if (env.enables <= env.enableFailures) return E_FAIL;
    env.preference = 1;
    return S_OK;
}

static NvspVtable vtable = {FakeRelease, NULL, NULL, FakeEnable, FakeDisable, FakeStatus};
static NvspApi fakeApi = {&vtable};

static HRESULT WINAPI FakeCreate(NvspCreate *args)
{
    assert(args->version == 0x10018 && args->interfaceVersion == 0x10008);
    assert(args->client == 6 && args->reserved == 0 && args->api && *args->api == NULL);
    ++env.creates;
    if (FAILED(env.createError)) return env.createError;
    if (env.malformedInterface) vtable.enable = NULL;
    *args->api = &fakeApi;
    return S_OK;
}

static FARPROC WINAPI FakeGetProcAddress(HMODULE library, LPCSTR name)
{
    assert(library == fakeLibrary && strcmp(name, "CreateShadowPlayApiInterface") == 0);
    union { FARPROC symbol; NvspCreateFn call; } create;
    create.call = FakeCreate;
    return env.missingExport ? NULL : create.symbol;
}

static BOOL WINAPI FakeFreeLibrary(HMODULE library)
{
    assert(library == fakeLibrary);
    ++env.frees;
    return TRUE;
}

static BOOL Allowed(void)
{
    ++env.permissionChecks;
    return env.allowed;
}

static void Reset(void)
{
    memset(&env, 0, sizeof(env));
    env.preference = 1;
    env.runtimeEnabled = env.userEnabled = 1;
    env.status = NVSP_STATE_RUNNING;
    env.allowed = TRUE;
    env.installPath = L"C:\\Custom NVIDIA\\CEF\\NVIDIA App.exe";
    vtable.enable = FakeEnable;
}

static OverlayRepairResult Repair(HRESULT expectedError)
{
    HRESULT error = E_UNEXPECTED;
    const OverlayRepairResult result = RepairNvidiaOverlay(Allowed, &error);
    assert(error == expectedError);
    assert(env.disables <= 1 && env.enables <= 2);
    assert(env.frees == env.loads - (env.missingLibrary ? 1u : 0u));
    assert(env.releases == env.creates - (FAILED(env.createError) ? 1u : 0u));
    return result;
}

int main(void)
{
    Reset();
    assert(Repair(S_OK) == OVERLAY_REPAIR_OK);
    assert(env.disables == 1 && env.enables == 1 && env.preference == 1);
    assert(env.permissionChecks == 2 && env.queries == 1);

    Reset();
    env.preference = 0;
    assert(Repair(S_OK) == OVERLAY_REPAIR_DISABLED);
    assert(env.loads == 0 && env.disables == 0 && env.enables == 0);

    Reset();
    env.preferenceError = ERROR_FILE_NOT_FOUND;
    assert(Repair(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) == OVERLAY_REPAIR_UNAVAILABLE);
    assert(env.loads == 0);
    Reset();
    env.shortPreference = TRUE;
    assert(Repair(HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) == OVERLAY_REPAIR_UNAVAILABLE);
    Reset();
    env.preference = 2;
    assert(Repair(HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) == OVERLAY_REPAIR_UNAVAILABLE);

    Reset();
    env.missingLibrary = TRUE;
    assert(Repair(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)) == OVERLAY_REPAIR_UNAVAILABLE);
    Reset();
    env.missingExport = TRUE;
    assert(Repair(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)) == OVERLAY_REPAIR_UNAVAILABLE);
    Reset();
    env.createError = E_NOINTERFACE;
    assert(Repair(E_NOINTERFACE) == OVERLAY_REPAIR_UNAVAILABLE);
    Reset();
    env.malformedInterface = TRUE;
    assert(Repair(E_NOINTERFACE) == OVERLAY_REPAIR_UNAVAILABLE);

    Reset();
    env.queryError = E_FAIL;
    assert(Repair(E_FAIL) == OVERLAY_REPAIR_UNAVAILABLE);
    assert(env.disables == 0 && env.enables == 0);
    const DWORD invalidStates[] = {0, 99};
    for (unsigned i = 0; i < _countof(invalidStates); ++i)
    {
        Reset();
        env.status = invalidStates[i];
        assert(Repair(HRESULT_FROM_WIN32(ERROR_NOT_READY)) == OVERLAY_REPAIR_UNAVAILABLE);
        assert(env.disables == 0 && env.enables == 0);
    }

    // RDP can leave the backend idle/stopped/disabled while the user's NVIDIA
    // preference remains ON. Requiring runtime ON would miss the repair case.
    for (DWORD state = NVSP_STATE_NOT_RUNNING; state <= NVSP_STATE_DISABLED; ++state)
    {
        Reset();
        env.status = state;
        env.runtimeEnabled = 0;
        assert(Repair(S_OK) == OVERLAY_REPAIR_OK && env.disables == 1 && env.enables == 1);
    }
    Reset();
    env.userEnabled = 0;
    assert(Repair(S_OK) == OVERLAY_REPAIR_DISABLED && env.disables == 0);
    Reset();
    env.runtimeEnabled = 2;
    assert(Repair(HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) == OVERLAY_REPAIR_UNAVAILABLE && env.disables == 0);

    Reset();
    env.allowed = FALSE;
    assert(Repair(S_OK) == OVERLAY_REPAIR_CANCELLED && env.loads == 0);
    Reset();
    env.cancelAfterStatus = TRUE;
    assert(Repair(S_OK) == OVERLAY_REPAIR_CANCELLED && env.disables == 0);
    Reset();
    env.disableAfterStatus = TRUE;
    assert(Repair(S_OK) == OVERLAY_REPAIR_DISABLED && env.disables == 0 && env.enables == 0);

    Reset();
    env.cancelDuringDisable = TRUE; // Finish restoration even when paused/exiting.
    assert(Repair(S_OK) == OVERLAY_REPAIR_OK && env.preference == 1 && env.enables == 1);
    Reset();
    env.disableError = E_FAIL; // Partial disable must not strand the overlay off.
    assert(Repair(E_FAIL) == OVERLAY_REPAIR_FAILED && env.preference == 1 && env.enables == 1);
    Reset();
    env.enableFailures = 1;
    assert(Repair(S_OK) == OVERLAY_REPAIR_OK && env.preference == 1 && env.enables == 2);
    Reset();
    env.enableFailures = 2;
    assert(Repair(E_FAIL) == OVERLAY_REPAIR_RESTORE_FAILED && env.enables == 2);

    Reset();
    env.installError = ERROR_FILE_NOT_FOUND;
    assert(Repair(S_OK) == OVERLAY_REPAIR_OK); // Default installation fallback.
    Reset();
    env.installPath = L"NVIDIA App.exe";
    assert(Repair(HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME)) == OVERLAY_REPAIR_UNAVAILABLE && env.loads == 0);
    Reset();
    env.installPath = L"\\\\server\\NVIDIA\\CEF\\NVIDIA App.exe";
    assert(Repair(HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME)) == OVERLAY_REPAIR_UNAVAILABLE && env.loads == 0);

    puts("NVIDIA overlay API tests passed (all native operations simulated).");
    return 0;
}
