#include "remote_hook.h"
#include "memory.h"
#include "utils.h"

#include <array>
#include <algorithm>
#include <cstring>

RemoteHook::RemoteHook(HANDLE process, uintptr_t targetAddress, size_t overwriteSize,
                       const std::vector<uint8_t>& stubBytes, const std::vector<uint8_t>& expectedOriginal)
    : hProcess_(process), targetAddress_(targetAddress), overwriteSize_(overwriteSize),
      stubBytes_(stubBytes), expectedOriginal_(expectedOriginal) {
}

bool RemoteHook::readTargetBytes(std::vector<uint8_t>& out) const {
    out.assign(overwriteSize_, 0);
    return readMemory(hProcess_, targetAddress_, out);
}

uintptr_t RemoteHook::getJumpDestination(const std::vector<uint8_t>& currentBytes) const {
    if (currentBytes.size() < 5 || currentBytes[0] != 0xE9) {
        return 0;
    }
    int32_t offset;
    std::memcpy(&offset, currentBytes.data() + 1, sizeof(offset));
    return targetAddress_ + 5 + offset;
}

bool RemoteHook::isOurHook(const std::vector<uint8_t>& currentBytes) const {
    if (currentBytes.size() < 5 || currentBytes[0] != 0xE9 || stubBytes_.empty() ||
        !std::all_of(currentBytes.begin() + 5, currentBytes.end(), [](uint8_t byte) { return byte == 0x90; })) {
        return false;
    }
    const uintptr_t destination = getJumpDestination(currentBytes);
    if (destination == 0) {
        return false;
    }

    std::vector<uint8_t> stubData(stubBytes_.size());
    if (!readMemory(hProcess_, destination, stubData)) {
        return false;
    }
    return stubData == stubBytes_;
}

PatchState RemoteHook::detect() const {
    if (overwriteSize_ < 5 || overwriteSize_ > 64 || stubBytes_.empty()) return PatchState::PatchedUnknown;
    std::vector<uint8_t> currentBytes;
    if (!readTargetBytes(currentBytes)) {
        return PatchState::PatchedUnknown;
    }

    if (isOurHook(currentBytes)) {
        return PatchState::PatchedByUs;
    }

    return currentBytes == expectedOriginal_ ? PatchState::Unpatched : PatchState::PatchedUnknown;
}

bool RemoteHook::install(AppliedPatch& out) {
    std::vector<uint8_t> originalBytes;
    if (!readTargetBytes(originalBytes)) {
        return false;
    }

    const PatchState state = detect();
    if (state == PatchState::PatchedByUs) {
        // A second call must not save the installed jump as "original" code.
        return true;
    }
    if (state == PatchState::PatchedUnknown) {
        return false;
    }

    const uintptr_t trampolineAddress = allocateMemoryNearAddress(hProcess_, targetAddress_, 0x1000);
    if (!trampolineAddress) {
        return false;
    }

    if (!writeMemoryWithProtectionDynamic(hProcess_, trampolineAddress, stubBytes_)) {
        freeRemoteMemory(hProcess_, trampolineAddress);
        return false;
    }

    std::array<uint8_t, 5> jmpInstructionBytes{};
    if (!assembleJumpNearInstruction(jmpInstructionBytes.data(), targetAddress_, trampolineAddress)) {
        freeRemoteMemory(hProcess_, trampolineAddress);
        return false;
    }

    std::vector<uint8_t> hookBytes(overwriteSize_, 0x90);
    std::copy(jmpInstructionBytes.begin(), jmpInstructionBytes.end(), hookBytes.begin());

    RemoteCodeGuard guard(hProcess_, targetAddress_, overwriteSize_);
    if (!guard.ready() || !readTargetBytes(originalBytes) || originalBytes != expectedOriginal_) {
        freeRemoteMemory(hProcess_, trampolineAddress);
        return false;
    }

    out.targetAddress = targetAddress_;
    out.originalBytes = originalBytes;
    out.patchedBytes = hookBytes;
    out.trampolineAddress = trampolineAddress;
    out.processId = GetProcessId(hProcess_);
    out.processCreated = getProcessCreationTime(hProcess_);
    if (!writeMemoryWithProtection(hProcess_, targetAddress_, hookBytes.data(), hookBytes.size())) {
        // Preserve recovery data if even the rollback fails; never free a live jump destination.
        if (writeMemoryWithProtection(hProcess_, targetAddress_, originalBytes.data(), originalBytes.size())) {
            freeRemoteMemory(hProcess_, trampolineAddress);
            out = {};
        }
        return false;
    }
    return true;
}

bool RemoteHook::uninstall(const AppliedPatch& patch) {
    if (patch.originalBytes.empty() || patch.targetAddress != targetAddress_ ||
        (patch.processId && patch.processId != GetProcessId(hProcess_)) ||
        (patch.processCreated && patch.processCreated != getProcessCreationTime(hProcess_))) return false;
    RemoteCodeGuard guard(hProcess_, targetAddress_, patch.originalBytes.size(),
                          patch.trampolineAddress, stubBytes_.size());
    if (!guard.ready()) return false;
    std::vector<uint8_t> current(patch.originalBytes.size());
    if (!readMemory(hProcess_, targetAddress_, current)) return false;
    if (current != patch.originalBytes) {
        if (current != patch.patchedBytes || !isOurHook(current)) return false;
        if (!writeMemoryWithProtection(hProcess_, targetAddress_, patch.originalBytes.data(),
                                       patch.originalBytes.size())) return false;
    }
    // Peer threads are still suspended and none was executing in the stub.
    return !patch.trampolineAddress || freeRemoteMemory(hProcess_, patch.trampolineAddress);
}
