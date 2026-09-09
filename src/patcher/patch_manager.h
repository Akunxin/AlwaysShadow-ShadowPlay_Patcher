#pragma once

#include "export_hook_patch.h"
#include "patch_config.h"
#include "patch_types.h"

#include <memory>
#include <string>
#include <vector>
#include <windows.h>

class PatchManager {
public:
    explicit PatchManager(PatchConfig config);

    bool loadStrategies(std::string& errorMessage);
    PatchReport applyAll(HANDLE proc);
    PatchReport undoAll(HANDLE proc);
    PatchReport undo(HANDLE proc, const std::string& id);
    PatchReport status(HANDLE proc);

    const std::vector<AppliedPatch>& appliedPatches() const { return appliedPatches_; }

private:
    PatchConfig config_;
    std::vector<std::unique_ptr<IPatchStrategy>> strategies_;
    std::vector<AppliedPatch> appliedPatches_;

    std::unique_ptr<IPatchStrategy> createStrategy(const PatchDefinition& definition);
    PatchResultEntry makeResult(const IPatchStrategy& strategy, PatchState state, PatchResultStatus status, const std::string& message);
    IPatchStrategy* findStrategy(const std::string& id);
};
