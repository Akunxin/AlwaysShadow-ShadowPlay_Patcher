#pragma once

#include "patch_types.h"

#include <memory>
#include <optional>
#include <string>
#include <windows.h>

class IPatchStrategy {
public:
    virtual ~IPatchStrategy() = default;
    virtual PatchState detect(HANDLE proc) = 0;
    virtual bool apply(HANDLE proc, AppliedPatch& out) = 0;
    virtual bool undo(HANDLE proc, const AppliedPatch& patch) = 0;
    virtual std::string id() const = 0;
    virtual bool required() const = 0;
};

class ExportHookPatch : public IPatchStrategy {
public:
    ExportHookPatch(const PatchDefinition& definition);

    PatchState detect(HANDLE proc) override;
    bool apply(HANDLE proc, AppliedPatch& out) override;
    bool undo(HANDLE proc, const AppliedPatch& patch) override;
    std::string id() const override { return definition_.id; }
    bool required() const override { return definition_.required; }

private:
    PatchDefinition definition_;
    std::optional<uintptr_t> resolveTarget(HANDLE proc) const;
    std::vector<uint8_t> originalExportBytes() const;
};
