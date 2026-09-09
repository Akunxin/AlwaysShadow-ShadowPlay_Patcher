#pragma once

#include "patch_types.h"

#include <cstdint>
#include <vector>
#include <windows.h>

class RemoteHook {
public:
    RemoteHook(HANDLE process, uintptr_t targetAddress, size_t overwriteSize,
               const std::vector<uint8_t>& stubBytes, const std::vector<uint8_t>& expectedOriginal = {});

    PatchState detect() const;
    bool install(AppliedPatch& out);
    bool uninstall(const AppliedPatch& patch);

private:
    HANDLE hProcess_;
    uintptr_t targetAddress_;
    size_t overwriteSize_;
    std::vector<uint8_t> stubBytes_;
    std::vector<uint8_t> expectedOriginal_;

    bool readTargetBytes(std::vector<uint8_t>& out) const;
    bool isOurHook(const std::vector<uint8_t>& currentBytes) const;
    uintptr_t getJumpDestination(const std::vector<uint8_t>& currentBytes) const;
};
