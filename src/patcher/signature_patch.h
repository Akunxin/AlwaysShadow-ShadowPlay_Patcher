#pragma once

#include "export_hook_patch.h"

#include <optional>
#include <vector>

class SignaturePatch : public IPatchStrategy {
public:
    explicit SignaturePatch(const PatchDefinition& definition);

    PatchState detect(HANDLE proc) override;
    bool apply(HANDLE proc, AppliedPatch& out) override;
    bool undo(HANDLE proc, const AppliedPatch& patch) override;
    std::string id() const override { return definition_.id; }
    bool required() const override { return definition_.required; }

private:
    PatchDefinition definition_;
    std::vector<std::vector<std::optional<uint8_t>>> patterns_;
    mutable uintptr_t cachedAddress_ = 0;
    mutable uintptr_t cachedModule_ = 0;
    mutable uint64_t cachedProcessCreated_ = 0;

    std::optional<uintptr_t> resolveTarget(HANDLE proc) const;
    bool readOriginalBytes(HANDLE proc, uintptr_t address, std::vector<uint8_t>& out) const;
    bool bytesMatchPatch(const std::vector<uint8_t>& current) const;
};
