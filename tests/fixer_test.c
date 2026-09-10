// Use the real NVIDIA control code with fake registry, HTTP and input boundaries.
// No real registry writes, network requests or keyboard input are performed.
#include "defines.h"
#include "patcher.h"
#include "nvidia_overlay.h"
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include <tchar.h>
#include <curl/curl.h>

GlobalCb glbl = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .loglock = PTHREAD_MUTEX_INITIALIZER,
};

char *GetDateTimeStaticStr(void) { return "test"; }
char *GetLastErrorStaticStr(void) { return "simulated error"; }
const wchar_t *UiText(const wchar_t *english, const wchar_t *chinese) { (void)chinese; return english; }
void PatcherShutdown(void) {}

static struct
{
    DWORD replay;
    LSTATUS registryError;
    BOOL shortStateValue;
    BOOL desktopAvailable;
    BOOL remoteSession;
    BOOL physicalConfirmed;
    BOOL virtualInputDuringPost;
    BOOL virtualInputDuringPatch;
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
    ULONGLONG now;
    PatcherPolicy lastPolicy;
    unsigned overlayCalls;
    OverlayRepairResult overlayResult;
    BOOL disconnectDuringOverlay;
    BOOL disableDuringOverlay;
} env;

void PatcherTick(BOOL enabled, PatcherPolicy policy, BOOL reload)
{
    (void)enabled; (void)reload;
    env.lastPolicy = policy;
    if (env.virtualInputDuringPatch) env.physicalConfirmed = FALSE;
}

OverlayRepairResult RepairNvidiaOverlay(OverlayRepairAllowedFn allowed, HRESULT *error)
{
    *error = S_OK;
    if (!allowed()) return OVERLAY_REPAIR_CANCELLED;
    assert(env.lastPolicy == PATCH_WAITING_FOR_DESKTOP); // Restore patches first.
    ++env.overlayCalls;
    if (env.overlayResult == OVERLAY_REPAIR_OK) env.replay = 0;
    if (env.disconnectDuringOverlay) env.desktopAvailable = FALSE;
    if (env.disableDuringOverlay) glbl.isDisabled = TRUE;
    return env.overlayResult;
}

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
BOOL FakeIsRemoteSession(void) { return env.remoteSession; }
BOOL FakePhysicalInputIsConfirmed(void) { return env.physicalConfirmed; }
void FakePhysicalInputRequireConfirmation(void) { env.physicalConfirmed = FALSE; }
static ULONGLONG WINAPI FakeGetTickCount64(void) { return env.now; }

static errno_t FakeOpenWhitelist(FILE **file, const WCHAR *name, const WCHAR *mode)
{
    assert(wcscmp(name, L"Whitelist.txt") == 0 && wcscmp(mode, L"r") == 0);
    *file = NULL; // Tests never load the user's runtime rules.
    return ENOENT;
}

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
    if (env.virtualInputDuringPost) env.physicalConfirmed = FALSE;
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
#define IsRemoteSession FakeIsRemoteSession
#define PhysicalInputIsConfirmed FakePhysicalInputIsConfirmed
#define PhysicalInputRequireConfirmation FakePhysicalInputRequireConfirmation
#define GetTickCount64 FakeGetTickCount64
#define _wfopen_s FakeOpenWhitelist
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
    glbl.rdpOverlayRecoveryEnabled = glbl.rdpRecoveryRequested = glbl.issueWarning = FALSE;
    cb.overlayRepairPending = FALSE;
    env.desktopAvailable = TRUE;
    env.physicalConfirmed = TRUE;
    cb.isExclusiveExists = FALSE;
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

static ReplayRecovery StartLocalRecovery(void)
{
    ResetEnvironment();
    ReplayRecovery recovery = {0};
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 0 && env.lastPolicy == PATCH_WAITING_FOR_DESKTOP);
    env.now = REPLAY_SETTLE_MS;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 1 && env.replay == 1 && env.lastPolicy == PATCH_ALLOWED);
    return recovery;
}

static void TestRemoteRecoveryPolling(void)
{
    ResetEnvironment();
    ReplayRecovery recovery = {0};
    env.physicalConfirmed = FALSE; // Starting over Sunlogin on the console desktop.
    for (env.now = 0; env.now <= 120000; env.now += 2000)
        PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 0 && env.postCalls == 0 && env.lastPolicy == PATCH_WAITING_FOR_DESKTOP);

    env.physicalConfirmed = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    env.now += REPLAY_SETTLE_MS - 1;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 0);
    ++env.now;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 1 && env.replay == 1);

    recovery = StartLocalRecovery();
    env.now = 12000;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(recovery.attempts == 1);
    env.now = 16000;
    env.replay = 0; // NVIDIA briefly enabled replay, then remote capture blocked it.
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(!env.physicalConfirmed && env.inputCalls == 1 && env.lastPolicy == PATCH_WAITING_FOR_DESKTOP);

    for (env.now = 20000; env.now < 86400000; env.now += 30000)
        PollReplayRecovery(&recovery, FALSE, TRUE); // Display/session events cannot re-enable.
    PollReplayRecovery(&recovery, TRUE, FALSE); // Nor settings reload.
    glbl.isDisabled = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    glbl.isDisabled = FALSE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 1 && env.postCalls == 0 && env.lastPolicy == PATCH_WAITING_FOR_DESKTOP);

    env.physicalConfirmed = TRUE; // Hardware input at the physical PC releases the gate.
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 1);
    env.now += REPLAY_SETTLE_MS;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 2 && env.replay == 1 && env.lastPolicy == PATCH_ALLOWED);

    ResetEnvironment();
    ZeroMemory(&recovery, sizeof(recovery));
    env.rejectInput = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    env.now = REPLAY_SETTLE_MS;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 1 && env.replay == 0);
    env.now += 2000;
    PollReplayRecovery(&recovery, TRUE, FALSE); // Reload cannot forget an unconfirmed enable.
    assert(recovery.awaitingEnable && env.inputCalls == 1);
    glbl.isDisabled = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    glbl.isDisabled = FALSE;
    assert(recovery.awaitingEnable);
    PollReplayRecovery(&recovery, FALSE, TRUE); // Nor can a display change.
    env.now += REPLAY_FAST_RETRY_MS;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(!env.physicalConfirmed && env.inputCalls == 1 && env.lastPolicy == PATCH_WAITING_FOR_DESKTOP);
    env.now += 86400000;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 1);

    recovery = StartLocalRecovery();
    env.desktopAvailable = FALSE; // RDP takes over an authorized local session.
    env.now = 12000;
    PollReplayRecovery(&recovery, FALSE, TRUE);
    assert(!env.physicalConfirmed && env.lastPolicy == PATCH_WAITING_FOR_DESKTOP);
    env.desktopAvailable = TRUE;
    env.replay = 0;
    env.now = 60000;
    PollReplayRecovery(&recovery, FALSE, TRUE);
    assert(env.inputCalls == 1 && env.lastPolicy == PATCH_WAITING_FOR_DESKTOP);
    env.physicalConfirmed = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    env.now += REPLAY_SETTLE_MS;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 2 && env.replay == 1);
}

static void TestExclusiveStopAndLateVirtualInput(void)
{
    ResetEnvironment();
    ReplayRecovery recovery = {0};
    cb.isExclusiveExists = TRUE;
    env.replay = 1;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    env.now = REPLAY_SETTLE_MS;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 1 && env.replay == 0 && env.lastPolicy == PATCH_OUTSIDE_EXCLUSIVES);
    env.now += 2000;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.physicalConfirmed && recovery.attempts == 0);
    cb.isExclusiveExists = FALSE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 2 && env.replay == 1);

    ResetEnvironment();
    ZeroMemory(&recovery, sizeof(recovery));
    PollReplayRecovery(&recovery, FALSE, FALSE);
    env.now = REPLAY_SETTLE_MS;
    env.virtualInputDuringPatch = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 0 && env.postCalls == 0); // Recheck before the command.
    env.now += 2000;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.lastPolicy == PATCH_WAITING_FOR_DESKTOP);
}

static void TestRdpZeroSecondReplayRepair(void)
{
    ReplayRecovery recovery = StartLocalRecovery();
    glbl.rdpOverlayRecoveryEnabled = TRUE;
    env.remoteSession = TRUE;
    env.desktopAvailable = FALSE;
    env.now += 2000;
    PollReplayRecovery(&recovery, FALSE, TRUE);
    assert(cb.overlayRepairPending && env.overlayCalls == 0 && !env.physicalConfirmed);

    // NVIDIA still reports ON, although its capture buffer is stuck at zero.
    env.remoteSession = FALSE;
    env.desktopAvailable = TRUE;
    env.now += 60000;
    PollReplayRecovery(&recovery, FALSE, TRUE);
    assert(env.replay == 1 && env.overlayCalls == 0);
    env.physicalConfirmed = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    env.now += REPLAY_SETTLE_MS - 1;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 0);
    ++env.now;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 1 && env.replay == 0 && env.inputCalls == 1);
    assert(!cb.overlayRepairPending && !recovery.awaitingEnable && env.physicalConfirmed);
    assert(env.lastPolicy == PATCH_WAITING_FOR_DESKTOP);

    // The deliberate stop is not a replay failure. Reload only after settling.
    env.now += 2000;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 1);
    env.now += REPLAY_SETTLE_MS;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.inputCalls == 2 && env.replay == 1 && env.physicalConfirmed);
    for (unsigned i = 0; i < 10; ++i)
    {
        env.now += 2000;
        PollReplayRecovery(&recovery, i == 2, i == 4); // Reload/display changes do not repeat the reset.
    }
    assert(env.overlayCalls == 1);

    // A new RDP episode schedules one new reset.
    env.remoteSession = TRUE;
    env.desktopAvailable = FALSE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    env.remoteSession = FALSE;
    env.desktopAvailable = env.physicalConfirmed = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    env.now += REPLAY_SETTLE_MS;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 2);
}

static ReplayRecovery ReadyForOverlayRepair(void)
{
    ResetEnvironment();
    env.now = 10000;
    env.replay = 1;
    glbl.rdpOverlayRecoveryEnabled = glbl.rdpRecoveryRequested = TRUE;
    const ReplayRecovery recovery = {.desktopAvailable = true};
    return recovery;
}

static void TestOverlayRepairPolicyAndCancellation(void)
{
    ReplayRecovery recovery = ReadyForOverlayRepair();
    glbl.rdpOverlayRecoveryEnabled = FALSE; // Default opt-out, including a latched WTS event.
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 0 && !cb.overlayRepairPending);

    recovery = ReadyForOverlayRepair();
    glbl.isDisabled = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 0 && cb.overlayRepairPending && env.lastPolicy == PATCH_PAUSED_BY_USER);
    glbl.isDisabled = FALSE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 1);

    recovery = ReadyForOverlayRepair();
    cb.isExclusiveExists = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 0 && cb.overlayRepairPending && env.lastPolicy == PATCH_OUTSIDE_EXCLUSIVES);
    cb.isExclusiveExists = FALSE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 1);

    recovery = ReadyForOverlayRepair();
    cb.whitelist = calloc(1, sizeof(*cb.whitelist));
    assert(cb.whitelist);
    cb.nwhitelist = 1;
    cb.whitelist[0].checkValue = SysAllocString(L"sample.exe");
    cb.whitelist[0].checkField = PROCFIELD_NAME;
    ResetWmi();
    wmi.hasProcess = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 0 && cb.overlayRepairPending && env.lastPolicy == PATCH_WHITELISTED);
    ResetWmi();
    wmi.queryResult = E_FAIL; // Incomplete rules cannot authorize a reset either.
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 0 && cb.overlayRepairPending);
    ResetWmi();
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 1);

    recovery = ReadyForOverlayRepair();
    env.virtualInputDuringPatch = TRUE; // Input authority changes just before native work.
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 0 && cb.overlayRepairPending);
    glbl.rdpOverlayRecoveryEnabled = FALSE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(!cb.overlayRepairPending && env.inputCalls == 0);

    recovery = ReadyForOverlayRepair();
    env.disconnectDuringOverlay = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    env.now += 60000;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 1 && env.inputCalls == 0 && !env.physicalConfirmed);

    recovery = ReadyForOverlayRepair();
    env.disableDuringOverlay = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    env.now += 60000;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 1 && env.inputCalls == 0 && glbl.isDisabled);
}

static void TestOverlayRepairFailuresAndStartup(void)
{
    const OverlayRepairResult results[] = {OVERLAY_REPAIR_DISABLED, OVERLAY_REPAIR_UNAVAILABLE,
        OVERLAY_REPAIR_FAILED, OVERLAY_REPAIR_RESTORE_FAILED};
    for (unsigned i = 0; i < _countof(results); ++i)
    {
        ReplayRecovery recovery = ReadyForOverlayRepair();
        env.overlayResult = results[i];
        PollReplayRecovery(&recovery, FALSE, FALSE);
        assert(env.overlayCalls == 1 && !cb.overlayRepairPending && env.inputCalls == 0);
        for (unsigned j = 0; j < 20; ++j)
        {
            env.now += 2000;
            PollReplayRecovery(&recovery, FALSE, FALSE);
        }
        assert(env.overlayCalls == 1); // Failed/missing API does not create a restart loop.
        if (results[i] == OVERLAY_REPAIR_RESTORE_FAILED)
            assert(glbl.issueWarning && !env.physicalConfirmed);
    }

    ResetEnvironment();
    ReplayRecovery recovery = {0};
    glbl.rdpOverlayRecoveryEnabled = TRUE;
    env.remoteSession = TRUE;
    env.desktopAvailable = FALSE;
    PollReplayRecovery(&recovery, FALSE, FALSE); // Startup in RDP without a WTS event.
    assert(cb.overlayRepairPending && env.overlayCalls == 0);
    env.remoteSession = FALSE;
    env.desktopAvailable = env.physicalConfirmed = TRUE;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    env.now += REPLAY_SETTLE_MS;
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 1);

    recovery = ReadyForOverlayRepair();
    // A WTS notification catches a short remote visit between polls; repeated
    // notifications coalesce into one pending repair.
    PollReplayRecovery(&recovery, FALSE, FALSE);
    assert(env.overlayCalls == 1 && !glbl.rdpRecoveryRequested);

    ResetEnvironment();
    ZeroMemory(&recovery, sizeof(recovery));
    glbl.rdpOverlayRecoveryEnabled = TRUE;
    PollReplayRecovery(&recovery, FALSE, TRUE); // Ordinary local startup/unlock.
    env.now += REPLAY_SETTLE_MS;
    PollReplayRecovery(&recovery, TRUE, FALSE);
    assert(env.overlayCalls == 0 && !cb.overlayRepairPending);
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
    env.physicalConfirmed = FALSE;
    ToggleInstantReplay(REPLAY_OFF);
    ToggleInstantReplay(REPLAY_ON);
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
    env.virtualInputDuringPost = TRUE;
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

    TestRemoteRecoveryPolling();
    TestExclusiveStopAndLateVirtualInput();
    TestRdpZeroSecondReplayRepair();
    TestOverlayRepairPolicyAndCancellation();
    TestOverlayRepairFailuresAndStartup();
    TestWhitelistQueries();
    ReleaseResources(FALSE);
    fclose(glbl.logfile);
    curl_global_cleanup();
    puts("NVIDIA control, remote recovery polling and whitelist query tests passed.");
    return 0;
}
