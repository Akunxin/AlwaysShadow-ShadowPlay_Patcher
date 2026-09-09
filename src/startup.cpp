#include "startup.h"
#include <taskschd.h>
#include <sddl.h>
#include <string>
#include <vector>

namespace {
template<class T> struct Com {
    T* value = nullptr;
    ~Com() { if (value) value->Release(); }
    T** out() { return &value; }
    T* operator->() const { return value; }
};
struct Bstr {
    BSTR value;
    explicit Bstr(const wchar_t* text) : value(SysAllocString(text)) {}
    ~Bstr() { SysFreeString(value); }
    operator BSTR() const { return value; }
};
struct ComSession {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComSession() { if (SUCCEEDED(result)) CoUninitialize(); }
};

std::wstring userSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    const BOOL ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!ok) return {};
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) return {};
    std::wstring result(sid);
    LocalFree(sid);
    return result;
}

HRESULT connect(Com<ITaskService>& service, Com<ITaskFolder>& folder) {
    HRESULT result = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ITaskService, reinterpret_cast<void**>(service.out()));
    if (FAILED(result)) return result;
    VARIANT empty;
    VariantInit(&empty);
    result = service->Connect(empty, empty, empty, empty);
    if (FAILED(result)) return result;
    return service->GetFolder(Bstr(L"\\"), folder.out());
}
} // namespace

extern "C" BOOL StartupIsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

extern "C" BOOL StartupTaskExists() {
    ComSession session;
    if (FAILED(session.result) && session.result != RPC_E_CHANGED_MODE) return FALSE;
    const auto sid = userSid();
    if (sid.empty()) return FALSE;
    Com<ITaskService> service;
    Com<ITaskFolder> folder;
    if (FAILED(connect(service, folder))) return FALSE;
    Com<IRegisteredTask> task;
    return SUCCEEDED(folder->GetTask(Bstr((L"AlwaysShadow-" + sid).c_str()), task.out()));
}

extern "C" BOOL StartupSetTask(BOOL enabled, wchar_t* error, size_t capacity) {
    auto fail = [&](HRESULT result) -> BOOL {
        if (error && capacity)
            swprintf_s(error, capacity, L"Task Scheduler error 0x%08lX. Run AlwaysShadow as administrator.", result);
        return FALSE;
    };
    ComSession session;
    if (FAILED(session.result) && session.result != RPC_E_CHANGED_MODE) return fail(session.result);
    const auto sid = userSid();
    if (sid.empty()) return fail(E_ACCESSDENIED);
    const std::wstring name = L"AlwaysShadow-" + sid;
    Com<ITaskService> service;
    Com<ITaskFolder> folder;
    HRESULT result = connect(service, folder);
    if (FAILED(result)) return fail(result);
    if (!enabled) {
        result = folder->DeleteTask(Bstr(name.c_str()), 0);
        return SUCCEEDED(result) || result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ? TRUE : fail(result);
    }
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
    if (!length || length >= _countof(path)) return fail(E_FAIL);
    std::wstring directory(path);
    directory.resize(directory.find_last_of(L"\\/"));
    Com<ITaskDefinition> definition;
    if (FAILED(result = service->NewTask(0, definition.out()))) return fail(result);
    Com<IRegistrationInfo> registration;
    if (FAILED(result = definition->get_RegistrationInfo(registration.out()))) return fail(result);
    if (FAILED(result = registration->put_Description(Bstr(L"Keep NVIDIA Instant Replay available for the signed-in user.")))) return fail(result);
    Com<IPrincipal> principal;
    if (FAILED(result = definition->get_Principal(principal.out()))) return fail(result);
    if (FAILED(result = principal->put_UserId(Bstr(sid.c_str()))) ||
        FAILED(result = principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
        FAILED(result = principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST))) return fail(result);
    Com<ITaskSettings> settings;
    if (FAILED(result = definition->get_Settings(settings.out()))) return fail(result);
    if (FAILED(result = settings->put_ExecutionTimeLimit(Bstr(L"PT0S"))) ||
        FAILED(result = settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) ||
        FAILED(result = settings->put_StopIfGoingOnBatteries(VARIANT_FALSE)) ||
        FAILED(result = settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW)) ||
        FAILED(result = settings->put_StartWhenAvailable(VARIANT_TRUE))) return fail(result);
    Com<ITriggerCollection> triggers;
    Com<ITrigger> trigger;
    Com<ILogonTrigger> logon;
    if (FAILED(result = definition->get_Triggers(triggers.out())) ||
        FAILED(result = triggers->Create(TASK_TRIGGER_LOGON, trigger.out())) ||
        FAILED(result = trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(logon.out()))) ||
        FAILED(result = logon->put_UserId(Bstr(sid.c_str())))) return fail(result);
    Com<IActionCollection> actions;
    Com<IAction> action;
    Com<IExecAction> exec;
    if (FAILED(result = definition->get_Actions(actions.out())) ||
        FAILED(result = actions->Create(TASK_ACTION_EXEC, action.out())) ||
        FAILED(result = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.out()))) ||
        FAILED(result = exec->put_Path(Bstr(path))) ||
        FAILED(result = exec->put_WorkingDirectory(Bstr(directory.c_str())))) return fail(result);
    VARIANT user, empty;
    VariantInit(&user);
    VariantInit(&empty);
    user.vt = VT_BSTR;
    user.bstrVal = SysAllocString(sid.c_str());
    Com<IRegisteredTask> registered;
    result = folder->RegisterTaskDefinition(Bstr(name.c_str()), definition.value, TASK_CREATE_OR_UPDATE,
        user, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, registered.out());
    VariantClear(&user);
    return SUCCEEDED(result) ? TRUE : fail(result);
}
