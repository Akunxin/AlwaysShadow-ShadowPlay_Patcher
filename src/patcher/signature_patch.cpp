#include "signature_patch.h"
#include "memory.h"
#include "signature_scanner.h"

#include <algorithm>

SignaturePatch::SignaturePatch(const PatchDefinition& definition)
    : definition_(definition), patterns_(definition.signatures) {}

std::optional<uintptr_t> SignaturePatch::resolveTarget(HANDLE proc) const {
    const auto module = getRemoteModuleInfo(proc, definition_.module.c_str());
    if (!module.baseAddress || !module.size) return std::nullopt;
    const auto created = getProcessCreationTime(proc);
    auto variants = patterns_;
    for (auto pattern : patterns_) {
        if (pattern.size() <= definition_.patchBytes.size()) continue;
        for (size_t i = 0; i < definition_.patchBytes.size(); ++i) pattern[i] = definition_.patchBytes[i];
        variants.push_back(std::move(pattern));
    }
    if (cachedAddress_ && cachedModule_ == module.baseAddress && cachedProcessCreated_ == created) {
        for (const auto& pattern : variants) {
            std::vector<uint8_t> current(pattern.size());
            if (!readMemory(proc, cachedAddress_, current)) break;
            bool matches = true;
            for (size_t i = 0; i < pattern.size(); ++i)
                if (pattern[i] && *pattern[i] != current[i]) { matches = false; break; }
            if (matches) return cachedAddress_;
        }
    }
    cachedAddress_ = 0;
    const auto match = scanModule(proc, module.baseAddress, module.size, variants);
    if (!match) return std::nullopt;
    cachedAddress_ = match->address;
    cachedModule_ = module.baseAddress;
    cachedProcessCreated_ = created;
    return cachedAddress_;
}

bool SignaturePatch::readOriginalBytes(HANDLE proc, uintptr_t address, std::vector<uint8_t>& out) const {
    out.resize(definition_.overwriteSize);
    return !out.empty() && readMemory(proc, address, out);
}

bool SignaturePatch::bytesMatchPatch(const std::vector<uint8_t>& current) const {
    return current == definition_.patchBytes;
}

PatchState SignaturePatch::detect(HANDLE proc) {
    const auto target = resolveTarget(proc);
    if (!target) return PatchState::NotFound;
    std::vector<uint8_t> current;
    if (!readOriginalBytes(proc, *target, current)) return PatchState::PatchedUnknown;
    return bytesMatchPatch(current) ? PatchState::PatchedByUs : PatchState::Unpatched;
}

bool SignaturePatch::apply(HANDLE proc, AppliedPatch& out) {
    const auto target = resolveTarget(proc);
    if (!target) return false;
    RemoteCodeGuard guard(proc, *target, definition_.overwriteSize);
    if (!guard.ready()) return false;
    std::vector<uint8_t> original;
    if (!readOriginalBytes(proc, *target, original)) return false;
    if (bytesMatchPatch(original)) return true;
    bool matchesOriginal = false;
    for (const auto& pattern : patterns_) {
        std::vector<uint8_t> bytes(pattern.size());
        if (!readMemory(proc, *target, bytes)) continue;
        bool matches = true;
        for (size_t i = 0; i < pattern.size(); ++i)
            if (pattern[i] && *pattern[i] != bytes[i]) { matches = false; break; }
        matchesOriginal |= matches;
    }
    if (!matchesOriginal) return false;
    out.id = definition_.id;
    out.type = PatchType::SignaturePatch;
    out.targetAddress = *target;
    out.originalBytes = original;
    out.patchedBytes = definition_.patchBytes;
    out.processId = GetProcessId(proc);
    out.processCreated = getProcessCreationTime(proc);
    if (!writeMemoryWithProtection(proc, *target, definition_.patchBytes.data(), definition_.patchBytes.size())) {
        if (writeMemoryWithProtection(proc, *target, original.data(), original.size())) out = {};
        return false;
    }
    return true;
}

bool SignaturePatch::undo(HANDLE proc, const AppliedPatch& patch) {
    if (patch.originalBytes.empty() || patch.processId != GetProcessId(proc) ||
        patch.processCreated != getProcessCreationTime(proc)) return false;
    const auto target = resolveTarget(proc);
    if (!target || *target != patch.targetAddress) return false;
    RemoteCodeGuard guard(proc, *target, patch.originalBytes.size());
    if (!guard.ready()) return false;
    std::vector<uint8_t> current(patch.originalBytes.size());
    if (!readMemory(proc, *target, current)) return false;
    if (current == patch.originalBytes) return true;
    if (current != patch.patchedBytes) return false;
    return writeMemoryWithProtection(proc, *target, patch.originalBytes.data(), patch.originalBytes.size());
}
