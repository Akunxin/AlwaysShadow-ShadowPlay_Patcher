// AlwaysShadow - integrated ShadowPlay protected-content patcher.
// Based on ShadowPlay_Patcher by furyzenblade.
// The patch is applied only to NVIDIA's running nvcontainer.exe process and does not modify files on disk.

#include "defines.h"
#include <tlhelp32.h>
#include <stdint.h>
#include <wchar.h>
#include <pthread.h>
#include <stdarg.h>

#if defined(_WIN64)

#define SHADOWPLAY_PROCESS_NAME L"nvcontainer.exe"
#define SHADOWPLAY_MARKER_MODULE L"nvd3dumx.dll"
#define PATCH_ALLOC_SIZE 0x1000
#define PATCH_SEARCH_RANGE ((SIZE_T)0x1fffe000)
#define PATCH_ALLOC_STEP ((uintptr_t)0x10000)

typedef enum
{
    PATCHER_STATE_UNKNOWN,
    PATCHER_STATE_DISABLED,
    PATCHER_STATE_NO_TARGET,
    PATCHER_STATE_MULTIPLE_TARGETS,
    PATCHER_STATE_OPEN_FAILED,
    PATCHER_STATE_PATCH_FAILED,
    PATCHER_STATE_PATCHED,
} PatcherState;

typedef enum
{
    REMOTE_PATCH_FAILED,
    REMOTE_PATCH_APPLIED,
    REMOTE_PATCH_ALREADY_PRESENT,
} RemotePatchResult;

static pthread_mutex_t patcherLock = PTHREAD_MUTEX_INITIALIZER;
static PatcherState lastState = PATCHER_STATE_UNKNOWN;
static DWORD lastPatchedPid = 0;

static char FindTargetShadowPlayProcess(DWORD *pidOut, DWORD *countOut);
static char IsModuleLoaded(DWORD processId, const wchar_t *moduleName);
static uintptr_t GetRemoteModuleBaseAddress(HANDLE process, const wchar_t *moduleName);
static uintptr_t GetExportedFunctionAddress(uintptr_t remoteModuleBase, const wchar_t *moduleName, const char *functionName);
static uintptr_t AllocateMemoryNearAddress(HANDLE process, uintptr_t desiredAddress, SIZE_T size);
static char AssembleNearJump(uint8_t *buffer, uintptr_t sourceAddress, uintptr_t targetAddress);
static char WriteMemory(HANDLE process, uintptr_t address, const void *buffer, SIZE_T size);
static char WriteMemoryWithProtection(HANDLE process, uintptr_t address, const void *buffer, SIZE_T size);
static RemotePatchResult PatchRemoteFunction(HANDLE process, const wchar_t *moduleName, const char *functionName, SIZE_T overwriteLength);
static void LogState(PatcherState state, char force, const char *fmt, ...);

void ApplyShadowPlayPatchIfEnabled(char force)
{
    pthread_mutex_lock(&patcherLock);

    pthread_mutex_lock(&glbl.lock);
    char isEnabled = glbl.isPatcherEnabled;
    pthread_mutex_unlock(&glbl.lock);

    if (!isEnabled)
    {
        LogState(PATCHER_STATE_DISABLED, force, "ShadowPlay patcher is disabled.");
        pthread_mutex_unlock(&patcherLock);
        return;
    }

    DWORD pid = 0;
    DWORD count = 0;

    if (!FindTargetShadowPlayProcess(&pid, &count))
    {
        if (count == 0)
        {
            LogState(PATCHER_STATE_NO_TARGET, force, "ShadowPlay patcher did not find NVIDIA's target nvcontainer.exe yet.");
        }
        else
        {
            LogState(PATCHER_STATE_MULTIPLE_TARGETS, force, "ShadowPlay patcher expected one target nvcontainer.exe with %ls loaded, found %lu.",
                SHADOWPLAY_MARKER_MODULE, (unsigned long)count);
        }

        pthread_mutex_unlock(&patcherLock);
        return;
    }

    if (!force && pid == lastPatchedPid && lastState == PATCHER_STATE_PATCHED)
    {
        pthread_mutex_unlock(&patcherLock);
        return;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);

    if (process == NULL)
    {
        LogState(PATCHER_STATE_OPEN_FAILED, force, "ShadowPlay patcher could not open target process %lu. LastError=%lu.",
            (unsigned long)pid, (unsigned long)GetLastError());
        pthread_mutex_unlock(&patcherLock);
        return;
    }

    RemotePatchResult affinityResult = PatchRemoteFunction(process, L"USER32.dll", "GetWindowDisplayAffinity", 6);
    RemotePatchResult moduleResult = REMOTE_PATCH_FAILED;

    if (affinityResult != REMOTE_PATCH_FAILED)
    {
        moduleResult = PatchRemoteFunction(process, L"KERNEL32.DLL", "Module32FirstW", 7);
    }

    CloseHandle(process);

    if (affinityResult == REMOTE_PATCH_FAILED || moduleResult == REMOTE_PATCH_FAILED)
    {
        LogState(PATCHER_STATE_PATCH_FAILED, force, "ShadowPlay patcher failed while patching target process %lu.", (unsigned long)pid);
        pthread_mutex_unlock(&patcherLock);
        return;
    }

    lastPatchedPid = pid;
    LogState(PATCHER_STATE_PATCHED, force,
        affinityResult == REMOTE_PATCH_ALREADY_PRESENT && moduleResult == REMOTE_PATCH_ALREADY_PRESENT
            ? "ShadowPlay patcher found target process %lu already patched/hooked."
            : "ShadowPlay patcher applied protected-content bypass to target process %lu.",
        (unsigned long)pid);

    pthread_mutex_unlock(&patcherLock);
}

static void LogState(PatcherState state, char force, const char *fmt, ...)
{
    if (!force && state == lastState)
    {
        return;
    }

    lastState = state;

    pthread_mutex_lock(&glbl.loglock);
    fprintf(glbl.logfile, "[INF] %s %s:%s:%d: ", GetDateTimeStaticStr(), __BASE_FILE__, __FUNCTION__, __LINE__);

    va_list args;
    va_start(args, fmt);
    vfprintf(glbl.logfile, fmt, args);
    va_end(args);

    fprintf(glbl.logfile, "\n");
    FFLUSH_DEBUG(glbl.logfile);
    pthread_mutex_unlock(&glbl.loglock);
}

static char FindTargetShadowPlayProcess(DWORD *pidOut, DWORD *countOut)
{
    *pidOut = 0;
    *countOut = 0;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }

    PROCESSENTRY32W entry = {0};
    entry.dwSize = sizeof(entry);

    if (!Process32FirstW(snapshot, &entry))
    {
        CloseHandle(snapshot);
        return FALSE;
    }

    do
    {
        if (_wcsicmp(entry.szExeFile, SHADOWPLAY_PROCESS_NAME) == 0 && IsModuleLoaded(entry.th32ProcessID, SHADOWPLAY_MARKER_MODULE))
        {
            *pidOut = entry.th32ProcessID;
            (*countOut)++;
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return *countOut == 1;
}

static char IsModuleLoaded(DWORD processId, const wchar_t *moduleName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);

    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }

    MODULEENTRY32W entry = {0};
    entry.dwSize = sizeof(entry);

    if (!Module32FirstW(snapshot, &entry))
    {
        CloseHandle(snapshot);
        return FALSE;
    }

    do
    {
        if (_wcsicmp(entry.szModule, moduleName) == 0)
        {
            CloseHandle(snapshot);
            return TRUE;
        }
    } while (Module32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return FALSE;
}

static uintptr_t GetRemoteModuleBaseAddress(HANDLE process, const wchar_t *moduleName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetProcessId(process));

    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    MODULEENTRY32W entry = {0};
    entry.dwSize = sizeof(entry);
    uintptr_t moduleBase = 0;

    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szModule, moduleName) == 0)
            {
                moduleBase = (uintptr_t)entry.modBaseAddr;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return moduleBase;
}

static uintptr_t GetExportedFunctionAddress(uintptr_t remoteModuleBase, const wchar_t *moduleName, const char *functionName)
{
    HMODULE localModule = LoadLibraryW(moduleName);

    if (localModule == NULL)
    {
        return 0;
    }

    FARPROC localFunction = GetProcAddress(localModule, functionName);

    if (localFunction == NULL)
    {
        FreeLibrary(localModule);
        return 0;
    }

    uintptr_t offset = (uintptr_t)localFunction - (uintptr_t)localModule;
    FreeLibrary(localModule);
    return remoteModuleBase + offset;
}

static uintptr_t AllocateMemoryNearAddress(HANDLE process, uintptr_t desiredAddress, SIZE_T size)
{
    uintptr_t start = desiredAddress > PATCH_SEARCH_RANGE ? desiredAddress - PATCH_SEARCH_RANGE : PATCH_ALLOC_STEP;
    uintptr_t end = UINTPTR_MAX - desiredAddress > PATCH_SEARCH_RANGE ? desiredAddress + PATCH_SEARCH_RANGE : UINTPTR_MAX;

    for (uintptr_t address = start; address < end; address += PATCH_ALLOC_STEP)
    {
        void *allocatedMemory = VirtualAllocEx(process, (void *)address, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);

        if (allocatedMemory != NULL)
        {
            return (uintptr_t)allocatedMemory;
        }

        if (UINTPTR_MAX - address < PATCH_ALLOC_STEP)
        {
            break;
        }
    }

    return 0;
}

static char AssembleNearJump(uint8_t *buffer, uintptr_t sourceAddress, uintptr_t targetAddress)
{
    intptr_t jumpOffset = (intptr_t)targetAddress - (intptr_t)(sourceAddress + 5);

    if (jumpOffset > INT32_MAX || jumpOffset < INT32_MIN)
    {
        return FALSE;
    }

    buffer[0] = 0xE9;
    *((int32_t *)(buffer + 1)) = (int32_t)jumpOffset;
    return TRUE;
}

static char WriteMemory(HANDLE process, uintptr_t address, const void *buffer, SIZE_T size)
{
    SIZE_T written = 0;
    return WriteProcessMemory(process, (void *)address, buffer, size, &written) && written == size;
}

static char WriteMemoryWithProtection(HANDLE process, uintptr_t address, const void *buffer, SIZE_T size)
{
    DWORD oldProtect = 0;

    if (!VirtualProtectEx(process, (void *)address, size, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        return FALSE;
    }

    char success = WriteMemory(process, address, buffer, size);
    FlushInstructionCache(process, (void *)address, size);

    DWORD unused = 0;
    if (!VirtualProtectEx(process, (void *)address, size, oldProtect, &unused))
    {
        return FALSE;
    }

    return success;
}

static RemotePatchResult PatchRemoteFunction(HANDLE process, const wchar_t *moduleName, const char *functionName, SIZE_T overwriteLength)
{
    uintptr_t remoteModuleBase = GetRemoteModuleBaseAddress(process, moduleName);

    if (remoteModuleBase == 0)
    {
        return REMOTE_PATCH_FAILED;
    }

    uintptr_t functionAddress = GetExportedFunctionAddress(remoteModuleBase, moduleName, functionName);

    if (functionAddress == 0)
    {
        return REMOTE_PATCH_FAILED;
    }

    uint8_t firstByte = 0;
    SIZE_T read = 0;

    if (ReadProcessMemory(process, (void *)functionAddress, &firstByte, sizeof(firstByte), &read) && read == sizeof(firstByte) && firstByte == 0xE9)
    {
        return REMOTE_PATCH_ALREADY_PRESENT;
    }

    uintptr_t allocatedMemory = AllocateMemoryNearAddress(process, functionAddress, PATCH_ALLOC_SIZE);

    if (allocatedMemory == 0)
    {
        return REMOTE_PATCH_FAILED;
    }

    const uint8_t payload[] = { 0x48, 0x31, 0xC0, 0xC3 }; // xor rax, rax; ret

    if (!WriteMemoryWithProtection(process, allocatedMemory, payload, sizeof(payload)))
    {
        return REMOTE_PATCH_FAILED;
    }

    uint8_t hook[8] = {0};

    if (overwriteLength > sizeof(hook) || !AssembleNearJump(hook, functionAddress, allocatedMemory))
    {
        return REMOTE_PATCH_FAILED;
    }

    for (SIZE_T i = 5; i < overwriteLength; i++)
    {
        hook[i] = 0x90;
    }

    if (!WriteMemoryWithProtection(process, functionAddress, hook, overwriteLength))
    {
        return REMOTE_PATCH_FAILED;
    }

    return REMOTE_PATCH_APPLIED;
}

#else

void ApplyShadowPlayPatchIfEnabled(char force)
{
    (void)force;
    static char logged = FALSE;

    if (!logged)
    {
        LOG_WARN("ShadowPlay patcher requires a 64-bit AlwaysShadow build and is disabled in this build.");
        logged = TRUE;
    }
}

#endif
