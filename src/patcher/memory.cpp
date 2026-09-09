#include "memory.h"
#include "utils.h"      // For toLower

#include <iostream>     // For std::cout
// TODO: Consider defining a Result struct instead to return error messages (E.g.: std::optional<Result> and return std::nullopt)
// instead of printing them to the console directly in the function.

#include <tlhelp32.h>   // CreateToolhelp32Snapshot
#include <algorithm>
#include <cstring>
#include <limits>

uint64_t getProcessCreationTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

RemoteCodeGuard::RemoteCodeGuard(HANDLE process, uintptr_t address, size_t size,
                                uintptr_t stub, size_t stubSize) {
    const DWORD pid = GetProcessId(process);
    if (!pid || !address || !size) return;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    size_t count = 0;
    if (Thread32First(snapshot, &entry)) {
        do { if (entry.th32OwnerProcessID == pid) ++count; } while (Thread32Next(snapshot, &entry));
    }
    // Allocate before suspending any thread (including tests targeting this process).
    threads_.reserve(count);
    bool ok = Thread32First(snapshot, &entry) != FALSE;
    if (ok) {
        do {
            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == GetCurrentThreadId()) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                       FALSE, entry.th32ThreadID);
            if (!thread) {
                if (GetLastError() == ERROR_INVALID_PARAMETER) continue; // Thread exited.
                ok = false;
                break;
            }
            if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
                CloseHandle(thread);
                ok = false;
                break;
            }
            threads_.push_back(thread);
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(thread, &context)) {
                ok = false;
                break;
            }
#if defined(_WIN64)
            const uintptr_t ip = context.Rip;
#else
#error The ShadowPlay patch engine requires a 64-bit build.
#endif
            if ((ip >= address && ip - address < size) ||
                (stub && ip >= stub && ip - stub < stubSize)) {
                ok = false;
                break;
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    ready_ = ok;
}

RemoteCodeGuard::~RemoteCodeGuard() {
    for (auto it = threads_.rbegin(); it != threads_.rend(); ++it) {
        ResumeThread(*it);
        CloseHandle(*it);
    }
}

std::vector<DWORD> getProcessesByName(const std::wstring& processName) {
    std::vector<DWORD> processIDs;
    HANDLE hProcessSnap;
    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);

    hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap == INVALID_HANDLE_VALUE) {
        return processIDs;
    }

    if (!Process32First(hProcessSnap, &pe32)) {
        CloseHandle(hProcessSnap);
        return processIDs;
    }

    std::wstring targetName = toLower(processName);
    do {
        if (toLower(pe32.szExeFile) == targetName) {
            processIDs.push_back(pe32.th32ProcessID);
        }
    } while (Process32Next(hProcessSnap, &pe32));

    CloseHandle(hProcessSnap);
    return processIDs;
}

bool isModuleLoaded(DWORD processID, const std::wstring& moduleName) {
    HANDLE hModuleSnap = INVALID_HANDLE_VALUE;
    MODULEENTRY32 me32;
    me32.dwSize = sizeof(MODULEENTRY32);

    hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processID);
    if (hModuleSnap == INVALID_HANDLE_VALUE) {
        return false;
    }

    if (!Module32First(hModuleSnap, &me32)) {
        CloseHandle(hModuleSnap);
        return false;
    }

    std::wstring targetModuleName = toLower(moduleName);

    do {
        if (toLower(me32.szModule) == targetModuleName || toLower(me32.szExePath) == targetModuleName) {
            CloseHandle(hModuleSnap);
            return true;
        }
    } while (Module32Next(hModuleSnap, &me32));

    CloseHandle(hModuleSnap);
    return false;
}

uintptr_t getRemoteModuleBaseAddress(HANDLE hProcess, const wchar_t* moduleName) {
    return getRemoteModuleInfo(hProcess, moduleName).baseAddress;
}

RemoteModuleInfo getRemoteModuleInfo(HANDLE hProcess, const wchar_t* moduleName) {
    RemoteModuleInfo info;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetProcessId(hProcess));
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return info;
    }

    MODULEENTRY32 me;
    me.dwSize = sizeof(MODULEENTRY32);
    if (Module32First(hSnapshot, &me)) {
        do {
            if (_wcsicmp(me.szModule, moduleName) == 0) {
                info.baseAddress = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                info.size = me.modBaseSize;
                break;
            }
        } while (Module32Next(hSnapshot, &me));
    }
    CloseHandle(hSnapshot);
    return info;
}

uintptr_t getExportedFunctionAddress(HANDLE hProcess, uintptr_t moduleBase, const wchar_t* moduleName, const char* functionName) {
    // Load the specified module locally (will just increase reference count if already loaded)
    // Only used to get a handle to the module
    HMODULE hLocalModule = LoadLibraryExW(moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hLocalModule) return 0;

    // Get the address of the function in the local module
    FARPROC localProcAddress = GetProcAddress(hLocalModule, functionName);
    if (!localProcAddress) {
        FreeLibrary(hLocalModule);
        return 0;
    }

    // GetProcAddress can return an export forwarded into KernelBase or another DLL.
    // Compute the RVA relative to the actual owner, not the requested module.
    MEMORY_BASIC_INFORMATION ownerInfo{};
    wchar_t ownerPath[MAX_PATH]{};
    uintptr_t result = 0;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(localProcAddress), &ownerInfo, sizeof(ownerInfo)) &&
        GetModuleFileNameW(static_cast<HMODULE>(ownerInfo.AllocationBase), ownerPath, MAX_PATH)) {
        const wchar_t* ownerName = wcsrchr(ownerPath, L'\\');
        ownerName = ownerName ? ownerName + 1 : ownerPath;
        const auto remoteOwner = getRemoteModuleInfo(hProcess, ownerName);
        const uintptr_t offset = reinterpret_cast<uintptr_t>(localProcAddress) -
                                 reinterpret_cast<uintptr_t>(ownerInfo.AllocationBase);
        if (remoteOwner.baseAddress && offset < remoteOwner.size) result = remoteOwner.baseAddress + offset;
    }

    // Free the local module (decrease reference count)
    FreeLibrary(hLocalModule);

    // TODO: Maybe refactor to only return the offset
    // Address of the function in the remote module
    (void)moduleBase;
    return result;
}

uintptr_t allocateMemoryNearAddress(HANDLE process, uintptr_t desiredAddress, SIZE_T size, DWORD protection, SIZE_T range) {
    /* Default/optional args:
    // DWORD protection     = PAGE_EXECUTE_READWRITE
    // SIZE_T range         = 0x20000000 - 0x2000
    */

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t step = info.dwAllocationGranularity;
    const uintptr_t minimum = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    uintptr_t address = (std::max)(minimum, desiredAddress > range ? desiredAddress - range : minimum);
    address = (address + step - 1) & ~(step - 1);
    const uintptr_t end = (std::min)(maximum - size, desiredAddress + range);
    while (address < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &region, sizeof(region))) break;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (region.State == MEM_FREE && regionEnd > address && regionEnd - address >= size) {
            if (void* allocated = VirtualAllocEx(process, reinterpret_cast<void*>(address), size,
                                                MEM_RESERVE | MEM_COMMIT, protection))
                return reinterpret_cast<uintptr_t>(allocated);
        }
        if (regionEnd <= address) break;
        address = (regionEnd + step - 1) & ~(step - 1);
    }
    return 0;
}

bool assembleJumpNearInstruction(uint8_t* buffer, uintptr_t sourceAddress, uintptr_t targetAddress) {
    // TODO: Use dynamic byte array or force length of 5 bytes

    // Calculate the relative offset for the jump
    const int64_t jumpOffset = static_cast<int64_t>(targetAddress) - static_cast<int64_t>(sourceAddress + 5);

    if (jumpOffset < INT32_MIN || jumpOffset > INT32_MAX) {
        return false;
    }

    // Assemble the JMP instruction (E9 offset)
    buffer[0] = 0xE9; // JMP opcode
    const int32_t relative = static_cast<int32_t>(jumpOffset);
    std::memcpy(buffer + 1, &relative, sizeof(relative));

    return true;
}

bool freeRemoteMemory(HANDLE hProcess, uintptr_t address) {
    return VirtualFreeEx(hProcess, reinterpret_cast<void*>(address), 0, MEM_RELEASE) != 0;
}

bool readMemory(HANDLE hProcess, uintptr_t address, std::span<uint8_t> buffer) {
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(
        hProcess,
        reinterpret_cast<void*>(address),
        buffer.data(),
        buffer.size(),
        &bytesRead) && bytesRead == buffer.size();
}

bool readMemory(HANDLE hProcess, uintptr_t address, std::vector<uint8_t>& buffer) {
    return readMemory(hProcess, address, std::span<uint8_t>(buffer.data(), buffer.size()));
}

bool writeMemory(HANDLE hProcess, uintptr_t address, const void* buffer, SIZE_T size) {
    SIZE_T written;
    return WriteProcessMemory(hProcess, reinterpret_cast<void*>(address), buffer, size, &written) && written == size;
}

bool writeMemoryWithProtection(HANDLE hProcess, uintptr_t address, const void* buffer, SIZE_T size) {
    if (!address || !buffer || !size) return false;
    DWORD oldProtect;

    // Change memory protection to allow writing
    if (!VirtualProtectEx(hProcess, reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::cout << "Error: Could not change memory protection" << std::endl;
        return false;
    }

    bool success = writeMemory(hProcess, address, buffer, size);
    if (success) success = FlushInstructionCache(hProcess, reinterpret_cast<LPCVOID>(address), size) != FALSE;

    // Restore the original memory protection
    if (!VirtualProtectEx(hProcess, reinterpret_cast<void*>(address), size, oldProtect, &oldProtect)) {
        std::cout << "Error: Could not restore memory protection" << std::endl;
        return false;
    }

    return success;
}

bool writeMemoryWithProtectionDynamic(HANDLE hProcess, uintptr_t address, const std::vector<uint8_t>& buffer) {
    return writeMemoryWithProtection(hProcess, address, buffer.data(), buffer.size());
}
