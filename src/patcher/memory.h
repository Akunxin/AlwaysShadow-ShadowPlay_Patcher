#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <windows.h>

struct RemoteModuleInfo {
    uintptr_t baseAddress = 0;
    size_t size = 0;
};

uint64_t getProcessCreationTime(HANDLE process);

// Stop peer threads only around instruction writes. Refuse to patch while an
// instruction pointer is inside the bytes being replaced or a retiring stub.
class RemoteCodeGuard {
public:
    RemoteCodeGuard(HANDLE process, uintptr_t address, size_t size,
                    uintptr_t stub = 0, size_t stubSize = 0);
    ~RemoteCodeGuard();
    RemoteCodeGuard(const RemoteCodeGuard&) = delete;
    RemoteCodeGuard& operator=(const RemoteCodeGuard&) = delete;
    bool ready() const { return ready_; }
private:
    std::vector<HANDLE> threads_;
    bool ready_ = false;
};

std::vector<DWORD> getProcessesByName(const std::wstring& processName);
bool isModuleLoaded(DWORD processID, const std::wstring& moduleName);
uintptr_t getRemoteModuleBaseAddress(HANDLE hProcess, const wchar_t* moduleName);
RemoteModuleInfo getRemoteModuleInfo(HANDLE hProcess, const wchar_t* moduleName);
uintptr_t getExportedFunctionAddress(HANDLE hProcess, uintptr_t moduleBase, const wchar_t* moduleName, const char* functionName);
uintptr_t allocateMemoryNearAddress(HANDLE process, uintptr_t desiredAddress, SIZE_T size,
    DWORD protection = PAGE_EXECUTE_READWRITE, SIZE_T range = 0x20000000 - 0x2000);
bool freeRemoteMemory(HANDLE hProcess, uintptr_t address);
bool assembleJumpNearInstruction(uint8_t* buffer, uintptr_t sourceAddress, uintptr_t targetAddress);
bool readMemory(HANDLE hProcess, uintptr_t address, std::span<uint8_t> buffer);
bool readMemory(HANDLE hProcess, uintptr_t address, std::vector<uint8_t>& buffer);
bool writeMemory(HANDLE hProcess, uintptr_t address, const void* buffer, SIZE_T size);
bool writeMemoryWithProtection(HANDLE hProcess, uintptr_t address, const void* buffer, SIZE_T size);
bool writeMemoryWithProtectionDynamic(HANDLE hProcess, uintptr_t address, const std::vector<uint8_t>& buffer);
