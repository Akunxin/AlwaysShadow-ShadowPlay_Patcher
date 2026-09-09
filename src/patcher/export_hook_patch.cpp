#include "export_hook_patch.h"
#include "memory.h"
#include "remote_hook.h"

#include <vector>
#include <cstring>

std::vector<uint8_t> ExportHookPatch::originalExportBytes() const {
    HMODULE module = LoadLibraryExW(definition_.module.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return {};
    FARPROC function = GetProcAddress(module, definition_.exportName.c_str());
    std::vector<uint8_t> bytes;
    if (function) {
        const auto* start = reinterpret_cast<const uint8_t*>(function);
        bytes.assign(start, start + definition_.overwriteSize);
    }
    FreeLibrary(module);
    return bytes;
}

ExportHookPatch::ExportHookPatch(const PatchDefinition& definition)
    : definition_(definition) {
}

std::optional<uintptr_t> ExportHookPatch::resolveTarget(HANDLE proc) const {
    const RemoteModuleInfo moduleInfo = getRemoteModuleInfo(proc, definition_.module.c_str());
    if (moduleInfo.baseAddress == 0) {
        return std::nullopt;
    }
    const uintptr_t address = getExportedFunctionAddress(
        proc,
        moduleInfo.baseAddress,
        definition_.module.c_str(),
        definition_.exportName.c_str());
    if (address == 0) {
        return std::nullopt;
    }
    return address;
}

PatchState ExportHookPatch::detect(HANDLE proc) {
    const auto target = resolveTarget(proc);
    if (!target.has_value()) {
        return PatchState::NotFound;
    }

    RemoteHook hook(proc, target.value(), definition_.overwriteSize, definition_.stubBytes, originalExportBytes());
    return hook.detect();
}

bool ExportHookPatch::apply(HANDLE proc, AppliedPatch& out) {
    const auto target = resolveTarget(proc);
    if (!target.has_value()) {
        return false;
    }

    RemoteHook hook(proc, target.value(), definition_.overwriteSize, definition_.stubBytes, originalExportBytes());
    const PatchState state = hook.detect();
    if (state == PatchState::PatchedUnknown) {
        return false;
    }

    AppliedPatch patch;
    const bool success = hook.install(patch);
    out = patch;
    out.id = definition_.id;
    out.type = PatchType::ExportHook;
    return success;
}

bool ExportHookPatch::undo(HANDLE proc, const AppliedPatch& patch) {
    AppliedPatch working = patch;
    const auto target = resolveTarget(proc);
    if (!target || (working.targetAddress && working.targetAddress != *target)) return false;

    // Live undo without session originals: restore from the local module export bytes.
    if (working.originalBytes.empty() || working.originalBytes.size() != definition_.overwriteSize) {
        const auto target = resolveTarget(proc);
        if (!target.has_value()) {
            return false;
        }

        RemoteHook detector(proc, *target, definition_.overwriteSize, definition_.stubBytes, originalExportBytes());
        if (detector.detect() != PatchState::PatchedByUs) return false;
        HMODULE localModule = LoadLibraryExW(definition_.module.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!localModule) {
            return false;
        }
        FARPROC localProc = GetProcAddress(localModule, definition_.exportName.c_str());
        if (!localProc) {
            FreeLibrary(localModule);
            return false;
        }

        working.targetAddress = target.value();
        working.originalBytes.assign(
            reinterpret_cast<const uint8_t*>(localProc),
            reinterpret_cast<const uint8_t*>(localProc) + definition_.overwriteSize);
        FreeLibrary(localModule);

        if (working.trampolineAddress == 0) {
            RemoteHook probe(proc, target.value(), definition_.overwriteSize, definition_.stubBytes);
            std::vector<uint8_t> current(definition_.overwriteSize);
            if (readMemory(proc, target.value(), current) && current.size() >= 5 && current[0] == 0xE9) {
                int32_t offset;
                std::memcpy(&offset, current.data() + 1, sizeof(offset));
                working.trampolineAddress = target.value() + 5 + offset;
                working.patchedBytes = current;
            }
        }
    }
    if (!working.processId) working.processId = GetProcessId(proc);
    if (!working.processCreated) working.processCreated = getProcessCreationTime(proc);

    RemoteHook hook(proc, working.targetAddress, definition_.overwriteSize, definition_.stubBytes);
    return hook.uninstall(working);
}
