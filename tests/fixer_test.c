// Use the real NVIDIA control code with fake registry, HTTP and input boundaries.
// No real registry writes, network requests or keyboard input are performed.
#include "defines.h"
#include "patcher.h"
#include <assert.h>
#include <stdarg.h>
#include <tchar.h>
#include <curl/curl.h>

GlobalCb glbl = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .loglock = PTHREAD_MUTEX_INITIALIZER,
};

char *GetDateTimeStaticStr(void) { return "test"; }
char *GetLastErrorStaticStr(void) { return "simulated error"; }
void PatcherTick(BOOL enabled, PatcherPolicy policy, BOOL reload) {
    (void)enabled; (void)policy; (void)reload;
}
void PatcherShutdown(void) {}

static struct
{
    DWORD replay;
    LSTATUS registryError;
    BOOL shortStateValue;
    BOOL desktopAvailable;
    BOOL disconnectDuringPost;
    BOOL restoreDuringPost;
    BOOL disableDuringPost;
    BOOL stopDuringPost;
    BOOL rejectInput;
    CURLcode postResult;
    long httpStatus;
    unsigned postCalls;
    unsigned inputCalls;
    int missingHotkey;
    DWORD firstHotkey;
} env;

static LSTATUS WINAPI FakeRegGetValueW(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD flags,
    DWORD *type, void *data, DWORD *size)
{
    assert(key == HKEY_CURRENT_USER && subkey != NULL && type == NULL);
    assert(*size == sizeof(DWORD));
    if (wcscmp(value, L"{1B1D3DAA-601D-49E5-8508-81736CA28C6D}") == 0)
    {
        assert(flags == RRF_RT_DWORD); // Includes NVIDIA's four-byte REG_BINARY values.
        *(DWORD *)data = env.replay;
        *size = env.shortStateValue ? 1 : sizeof(DWORD);
        return env.registryError;
    }

    if (wcscmp(value, L"IRToggleHKeyCount") == 0)
    {
        *(DWORD *)data = 3;
        return ERROR_SUCCESS;
    }

    int index = -1;
    assert(swscanf(value, L"IRToggleHKey%d", &index) == 1);
    assert(index >= 0 && index < 3);
    if (index == env.missingHotkey) return ERROR_FILE_NOT_FOUND;
    DWORD keys[] = {env.firstHotkey, VK_SHIFT, VK_F10};
    *(DWORD *)data = keys[index];
    return ERROR_SUCCESS;
}

BOOL FakeIsLocalInteractiveSession(void) { return env.desktopAvailable; }

static HANDLE WINAPI FakeOpenFileMappingW(DWORD access, BOOL inherit, LPCWSTR name)
{
    assert(access == FILE_MAP_READ && !inherit && name != NULL);
    return NULL; // New NVIDIA App has no legacy shared-memory server.
}

static UINT WINAPI FakeSendInput(UINT count, INPUT *inputs, int size)
{
    assert(count == 6 && size == sizeof(INPUT));
    assert(inputs[0].ki.wVk == env.firstHotkey && inputs[0].ki.dwFlags == 0);
    assert(inputs[3].ki.dwFlags == KEYEVENTF_KEYUP);
    env.inputCalls++;
    if (env.rejectInput) return 0;
    env.replay = !env.replay;
    return count;
}

static CURLcode FakeCurlEasyPerform(CURL *handle)
{
    assert(handle != NULL);
    env.postCalls++;
    if (env.disconnectDuringPost) env.desktopAvailable = FALSE;
    if (env.restoreDuringPost) env.replay = 1;
    if (env.disableDuringPost) glbl.isDisabled = TRUE;
    if (env.stopDuringPost) glbl.isStopping = TRUE;
    return env.postResult;
}

static CURLcode FakeCurlEasyGetinfo(CURL *handle, CURLINFO info, ...)
{
    assert(handle != NULL && info == CURLINFO_RESPONSE_CODE);
    va_list args;
    va_start(args, info);
    *va_arg(args, long *) = env.httpStatus;
    va_end(args);
    return CURLE_OK;
}

#define RegGetValueW FakeRegGetValueW
#define IsLocalInteractiveSession FakeIsLocalInteractiveSession
#define OpenFileMappingW FakeOpenFileMappingW
#define SendInput FakeSendInput
#define curl_easy_perform FakeCurlEasyPerform
#undef curl_easy_getinfo
#define curl_easy_getinfo FakeCurlEasyGetinfo
#include "../src/fixer.c"

// Exercise the real whitelist polling with controlled WMI results. In
// particular, a failed or incomplete query must never be treated as no match.
static struct {
    HRESULT queryResult;
    HRESULT endResult;
    BOOL hasProcess;
    unsigned queries, nextCalls, processReleases, enumReleases;
} wmi;
static IWbemClassObject fakeProcess;
static IEnumWbemClassObject fakeEnumerator;

static HRESULT STDMETHODCALLTYPE FakeExecQuery(IWbemServices *self, const BSTR language,
        const BSTR query, LONG flags, IWbemContext *context, IEnumWbemClassObject **out)
{
    (void)self;
    assert(wcscmp(language, L"WQL") == 0 && wcsstr(query, L"Win32_Process"));
    assert((flags & WBEM_FLAG_RETURN_IMMEDIATELY) && !context);
    ++wmi.queries;
    *out = FAILED(wmi.queryResult) ? NULL : &fakeEnumerator;
    return wmi.queryResult;
}

static HRESULT STDMETHODCALLTYPE FakeNextProcess(IEnumWbemClassObject *self, LONG timeout,
        ULONG count, IWbemClassObject **out, ULONG *returned)
{
    (void)self;
    assert(timeout == 1000 && count == 1);
    ++wmi.nextCalls;
    *out = NULL;
    *returned = 0;
    if (wmi.hasProcess && wmi.nextCalls == 1) {
        *out = &fakeProcess;
        *returned = 1;
        return S_OK;
    }
    return wmi.endResult;
}

static HRESULT STDMETHODCALLTYPE FakeProcessField(IWbemClassObject *self, LPCWSTR name,
        LONG flags, VARIANT *value, CIMTYPE *type, LONG *flavor)
{
    (void)self;
    assert(!flags && !type && !flavor);
    value->vt = VT_BSTR;
    value->bstrVal = SysAllocString(wcscmp(name, L"Name") == 0 ? L"sample.exe" : L"sample.exe --test");
    assert(value->bstrVal);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE ReleaseFakeProcess(IWbemClassObject *self)
{
    (void)self;
    return ++wmi.processReleases;
}

static ULONG STDMETHODCALLTYPE ReleaseFakeEnumerator(IEnumWbemClassObject *self)
{
    (void)self;
    return ++wmi.enumReleases;
}

static IWbemServicesVtbl serviceVtable = {.ExecQuery = FakeExecQuery};
static IEnumWbemClassObjectVtbl enumVtable = {.Next = FakeNextProcess, .Release = ReleaseFakeEnumerator};
static IWbemClassObjectVtbl processVtable = {.Get = FakeProcessField, .Release = ReleaseFakeProcess};
static IWbemServices fakeServices = {.lpVtbl = &serviceVtable};
static IEnumWbemClassObject fakeEnumerator = {.lpVtbl = &enumVtable};
static IWbemClassObject fakeProcess = {.lpVtbl = &processVtable};

static void ResetWmi(void)
{
    memset(&wmi, 0, sizeof(wmi));
    wmi.endResult = WBEM_S_FALSE;
    cb.wbemServices = &fakeServices;
    glbl.isStopping = FALSE;
}

static void TestWhitelistQueries(void)
{
    WhitelistEntry entry = {.checkValue = SysAllocString(L"sample.exe"), .checkField = PROCFIELD_NAME};
    assert(entry.checkValue);
    char whitelisted, exclusive;

    ResetWmi();
    PollRunningProcesses(NULL, 0, &whitelisted, &exclusive);
    assert(!whitelisted && !exclusive && wmi.queries == 0);

    ResetWmi();
    wmi.queryResult = WBEM_E_FAILED;
    PollRunningProcesses(&entry, 1, &whitelisted, &exclusive);
    assert(whitelisted && !exclusive && wmi.nextCalls == 0);

    ResetWmi();
    wmi.endResult = WBEM_S_TIMEDOUT;
    PollRunningProcesses(&entry, 1, &whitelisted, &exclusive);
    assert(whitelisted && !exclusive && wmi.nextCalls == 1 && wmi.enumReleases == 1);

    ResetWmi();
    PollRunningProcesses(&entry, 1, &whitelisted, &exclusive);
    assert(!whitelisted && !exclusive && wmi.enumReleases == 1);

    ResetWmi();
    wmi.hasProcess = TRUE;
    PollRunningProcesses(&entry, 1, &whitelisted, &exclusive);
    assert(whitelisted && !exclusive && wmi.processReleases == 1 && wmi.enumReleases == 1);

    ResetWmi();
    wmi.hasProcess = TRUE;
    entry.isExclusive = TRUE;
    PollRunningProcesses(&entry, 1, &whitelisted, &exclusive);
    assert(!whitelisted && exclusive && wmi.processReleases == 1 && wmi.enumReleases == 1);

    ResetWmi();
    wmi.hasProcess = TRUE;
    glbl.isStopping = TRUE;
    PollRunningProcesses(&entry, 1, &whitelisted, &exclusive);
    assert(whitelisted && wmi.nextCalls == 1 && wmi.processReleases == 1 && wmi.enumReleases == 1);

    SysFreeString(entry.checkValue);
    cb.wbemServices = NULL;
    glbl.isStopping = FALSE;
}

static void ResetEnvironment(void)
{
    ReleaseResources(FALSE);
    memset(&env, 0, sizeof(env));
    glbl.isDisabled = glbl.sessionChanged = glbl.isStopping = FALSE;
    env.desktopAvailable = TRUE;
    env.missingHotkey = -1;
    env.firstHotkey = VK_CONTROL;
    env.postResult = CURLE_COULDNT_CONNECT;
    env.httpStatus = 503;
    cb.inputs = FetchToggleShortcut(&cb.ninputs);
    assert(cb.inputs != NULL && cb.ninputs == 6);
}

static void AddFakeLegacyServer(void)
{
    cb.curl = curl_easy_init();
    assert(cb.curl != NULL);
}

int main(void)
{
    assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
    glbl.logfile = tmpfile();
    assert(glbl.logfile != NULL);

    ResetEnvironment();
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.inputCalls == 1 && env.replay == 1 && env.postCalls == 0);

    ResetEnvironment();
    env.replay = 1; // NVIDIA has already restored replay since the poll.
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.inputCalls == 0 && env.replay == 1);

    ResetEnvironment();
    env.registryError = ERROR_FILE_NOT_FOUND;
    assert(GetInstantReplayState() == REPLAY_UNKNOWN);
    ToggleInstantReplay(REPLAY_OFF);
    ToggleInstantReplay(REPLAY_UNKNOWN);
    assert(env.inputCalls == 0 && env.postCalls == 0);

    ResetEnvironment();
    env.shortStateValue = TRUE;
    assert(GetInstantReplayState() == REPLAY_UNKNOWN);
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.inputCalls == 0);

    ResetEnvironment();
    glbl.isDisabled = TRUE;
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.inputCalls == 0 && env.postCalls == 0);

    ResetEnvironment();
    glbl.sessionChanged = TRUE;
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.inputCalls == 0 && env.postCalls == 0);

    ResetEnvironment();
    env.desktopAvailable = FALSE;
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.inputCalls == 0 && env.postCalls == 0);

    ResetEnvironment();
    glbl.isStopping = TRUE;
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.inputCalls == 0 && env.postCalls == 0);

    ResetEnvironment();
    AddFakeLegacyServer();
    env.disconnectDuringPost = TRUE;
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.postCalls == 1 && env.inputCalls == 0);

    ResetEnvironment();
    AddFakeLegacyServer();
    env.restoreDuringPost = TRUE; // HTTP failed, but replay has become enabled.
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.postCalls == 1 && env.inputCalls == 0 && env.replay == 1);

    ResetEnvironment();
    AddFakeLegacyServer();
    env.disableDuringPost = TRUE;
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.postCalls == 1 && env.inputCalls == 0);

    ResetEnvironment();
    AddFakeLegacyServer();
    env.stopDuringPost = TRUE;
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.postCalls == 1 && env.inputCalls == 0);

    ResetEnvironment();
    AddFakeLegacyServer();
    env.postResult = CURLE_OK;
    env.httpStatus = 200;
    env.restoreDuringPost = TRUE;
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.postCalls == 1 && env.inputCalls == 0 && env.replay == 1);

    ResetEnvironment();
    env.rejectInput = TRUE;
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.inputCalls == 1 && env.replay == 0);
    env.rejectInput = FALSE;
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.inputCalls == 2 && env.replay == 1);

    ResetEnvironment();
    env.missingHotkey = 1; // NVIDIA is rebuilding its registry values.
    ReloadReplayControls();
    assert(cb.inputs == NULL && cb.ninputs == 0 && !glbl.fixerDied);
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.inputCalls == 0);
    env.missingHotkey = -1;
    env.firstHotkey = VK_MENU;
    ReloadReplayControls();
    ToggleInstantReplay(REPLAY_OFF);
    assert(env.inputCalls == 1 && env.replay == 1);

    TestWhitelistQueries();
    ReleaseResources(FALSE);
    fclose(glbl.logfile);
    curl_global_cleanup();
    puts("NVIDIA control tests passed (15 recovery scenarios, 7 whitelist query scenarios).");
    return 0;
}
