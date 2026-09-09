#include "shadowplay_target.h"
#include "memory.h"
#include "utils.h"

#include <sstream>
#include <vector>
#include <winternl.h>

typedef NTSTATUS(NTAPI* NtQueryInformationProcessFn)(
    HANDLE ProcessHandle,
    PROCESSINFOCLASS ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength);

static bool enableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);
    return ok && err == ERROR_SUCCESS;
}

std::string getProcessCommandLine(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (!process) {
        return "";
    }

    auto ntQueryInformationProcess = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!ntQueryInformationProcess) {
        CloseHandle(process);
        return "";
    }

    constexpr PROCESSINFOCLASS kProcessCommandLineInformation = static_cast<PROCESSINFOCLASS>(60);

    ULONG returnLength = 0;
    NTSTATUS status = ntQueryInformationProcess(
        process,
        kProcessCommandLineInformation,
        nullptr,
        0,
        &returnLength);
    if (returnLength == 0) {
        CloseHandle(process);
        return "";
    }

    std::vector<uint8_t> buffer(returnLength);
    status = ntQueryInformationProcess(
        process,
        kProcessCommandLineInformation,
        buffer.data(),
        static_cast<ULONG>(buffer.size()),
        &returnLength);
    CloseHandle(process);

    if (status < 0 || returnLength < sizeof(UNICODE_STRING)) {
        return "";
    }

    const auto* commandLine = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
    if (commandLine->Buffer == nullptr || commandLine->Length == 0) {
        return "";
    }

    const size_t charCount = commandLine->Length / sizeof(wchar_t);
    return wstringToString(std::wstring(commandLine->Buffer, charCount));
}

static bool looksLikeShadowPlayModules(DWORD processId) {
    // nvd3dumx is the classic marker, but it may only appear while Instant Replay is active.
    static const wchar_t* kMarkers[] = {
        L"nvd3dumx.dll",
        L"nvspcaps64.dll",
        L"nvspcap64.dll",
        L"nvencodeapi64.dll",
    };
    for (const wchar_t* marker : kMarkers) {
        if (isModuleLoaded(processId, marker)) {
            return true;
        }
    }
    return false;
}

std::optional<ShadowPlayTarget> findShadowPlayProcess(std::string& errorMessage) {
    errorMessage.clear();
    enableDebugPrivilege();

    const std::vector<DWORD> processIds = getProcessesByName(L"nvcontainer.exe");
    if (processIds.empty()) {
        errorMessage =
            "No nvcontainer.exe processes found. Start the NVIDIA App / GeForce Experience "
            "and turn Instant Replay ON, then try again.";
        return std::nullopt;
    }

    std::vector<DWORD> spUserCandidates;
    std::vector<DWORD> moduleCandidates;
    int cmdlineReadable = 0;
    DWORD ownSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &ownSession)) {
        errorMessage = "Could not determine the current Windows session";
        return std::nullopt;
    }

    for (DWORD processId : processIds) {
        DWORD session = 0;
        if (!ProcessIdToSessionId(processId, &session) || session != ownSession) continue;
        const std::string cmd = getProcessCommandLine(processId);
        if (!cmd.empty()) {
            ++cmdlineReadable;
            if (cmd.find("SPUser") != std::string::npos ||
                cmd.find("\\plugins\\SPUser") != std::string::npos ||
                cmd.find("/plugins/SPUser") != std::string::npos) {
                spUserCandidates.push_back(processId);
            }
        }
        if (looksLikeShadowPlayModules(processId)) {
            moduleCandidates.push_back(processId);
        }
    }

    // Prefer SPUser process identity; module markers are only a fallback.
    // Prefer SPUser that also has capture modules when Instant Replay is fully up.
    std::vector<DWORD> selected;
    if (!spUserCandidates.empty()) {
        std::vector<DWORD> spWithModules;
        for (DWORD pid : spUserCandidates) {
            if (looksLikeShadowPlayModules(pid)) {
                spWithModules.push_back(pid);
            }
        }
        selected = !spWithModules.empty() ? spWithModules : spUserCandidates;
    }
    else if (!moduleCandidates.empty()) {
        selected = moduleCandidates;
    }
    else {
        std::ostringstream oss;
        oss << "Found " << processIds.size()
            << " nvcontainer.exe process(es), but none look like ShadowPlay "
               "(no SPUser cmdline / no capture modules).\r\n"
            << "Tips:\r\n"
            << "1) Turn Instant Replay ON in the NVIDIA overlay (Alt+Z)\r\n"
            << "2) Run this app as Administrator\r\n"
            << "3) Wait for automatic retry, or choose Reload settings";
        if (cmdlineReadable == 0) {
            oss << "\r\n(Could not read process command lines — elevation may be required.)";
        }
        errorMessage = oss.str();
        return std::nullopt;
    }

    // If multiple SPUser matches somehow, prefer one that also has capture modules.
    if (selected.size() > 1) {
        std::vector<DWORD> narrowed;
        for (DWORD pid : selected) {
            if (looksLikeShadowPlayModules(pid)) {
                narrowed.push_back(pid);
            }
        }
        if (narrowed.size() == 1) {
            selected = narrowed;
        }
        else if (!narrowed.empty()) {
            selected = narrowed;
        }
    }

    if (selected.size() != 1) {
        errorMessage = "Expected exactly one ShadowPlay nvcontainer process, found "
            + std::to_string(selected.size())
            + ". Close extra NVIDIA containers or reboot, then retry.";
        return std::nullopt;
    }

    const DWORD processId = selected.front();
    HANDLE processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
        PROCESS_VM_WRITE | PROCESS_VM_OPERATION | SYNCHRONIZE, FALSE, processId);
    if (!processHandle) {
        errorMessage = "Could not open nvcontainer.exe (PID " + std::to_string(processId)
            + "). Run as Administrator.";
        return std::nullopt;
    }

    ShadowPlayTarget target;
    target.processId = processId;
    target.processHandle = processHandle;
    return target;
}

void closeShadowPlayTarget(ShadowPlayTarget& target) {
    if (target.processHandle) {
        CloseHandle(target.processHandle);
        target.processHandle = nullptr;
    }
}
