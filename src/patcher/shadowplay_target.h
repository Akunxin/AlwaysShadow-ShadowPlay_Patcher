#pragma once

#include <optional>
#include <string>
#include <windows.h>

struct ShadowPlayTarget {
    DWORD processId = 0;
    HANDLE processHandle = nullptr;
};

struct ShadowPlayTargetError {
    std::string message;
    DWORD win32Error = 0;
};

std::optional<ShadowPlayTarget> findShadowPlayProcess(std::string& errorMessage);
void closeShadowPlayTarget(ShadowPlayTarget& target);
std::string getProcessCommandLine(DWORD processId);
