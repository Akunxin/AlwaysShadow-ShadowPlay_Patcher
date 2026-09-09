#include "patch_manager.h"
#include "signature_patch.h"
#include "memory.h"

#include <algorithm>

PatchManager::PatchManager(PatchConfig config) : config_(std::move(config)) {}

std::unique_ptr<IPatchStrategy> PatchManager::createStrategy(const PatchDefinition& definition) {
    if (definition.type == PatchType::ExportHook) return std::make_unique<ExportHookPatch>(definition);
    if (definition.type == PatchType::SignaturePatch) return std::make_unique<SignaturePatch>(definition);
    return nullptr;
}

bool PatchManager::loadStrategies(std::string& error) {
    if (!appliedPatches_.empty()) {
        error = "Restore existing patches before reloading strategies";
        return false;
    }
    strategies_.clear();
    for (const auto& definition : config_.patches) {
        if (!definition.enabled) continue;
        auto strategy = createStrategy(definition);
        if (!strategy) { error = "Unsupported patch type: " + definition.id; return false; }
        strategies_.push_back(std::move(strategy));
    }
    return true;
}

PatchResultEntry PatchManager::makeResult(const IPatchStrategy& strategy, PatchState state,
                                          PatchResultStatus status, const std::string& message) {
    return {strategy.id(), status, state, message};
}

IPatchStrategy* PatchManager::findStrategy(const std::string& id) {
    for (const auto& strategy : strategies_) if (strategy->id() == id) return strategy.get();
    return nullptr;
}

static bool sameProcess(HANDLE proc, const AppliedPatch& patch) {
    return patch.processId == GetProcessId(proc) && patch.processCreated &&
           patch.processCreated == getProcessCreationTime(proc);
}

PatchReport PatchManager::status(HANDLE proc) {
    PatchReport report;
    report.success = true;
    for (const auto& strategy : strategies_) {
        const auto state = strategy->detect(proc);
        const auto status = state == PatchState::PatchedByUs ? PatchResultStatus::AlreadyPatched :
                            state == PatchState::NotFound ? PatchResultStatus::NotFound : PatchResultStatus::Skipped;
        report.entries.push_back(makeResult(*strategy, state, status, ""));
        if (strategy->required() && state != PatchState::PatchedByUs) report.success = false;
    }
    return report;
}

PatchReport PatchManager::applyAll(HANDLE proc) {
    PatchReport report;
    report.success = true;
    // The caller must create a fresh manager after a target process exits.
    for (const auto& record : appliedPatches_) {
        if (!sameProcess(proc, record)) {
            report.entries.push_back({record.id, PatchResultStatus::Failed, PatchState::PatchedUnknown,
                                      "Process identity changed; refusing to use old addresses"});
            report.success = false;
            return report;
        }
    }
    for (const auto& strategy : strategies_) {
        const auto state = strategy->detect(proc);
        if (state == PatchState::PatchedByUs) {
            report.entries.push_back(makeResult(*strategy, state, PatchResultStatus::AlreadyPatched, "already patched"));
            continue;
        }
        if (state == PatchState::PatchedUnknown || state == PatchState::NotFound) {
            report.entries.push_back(makeResult(*strategy, state,
                strategy->required() ? PatchResultStatus::Failed : PatchResultStatus::Skipped,
                state == PatchState::NotFound ? "Target missing or signature is not unique" : "Unrecognized code; refusing to overwrite"));
            if (strategy->required()) report.success = false;
            continue;
        }
        auto record = std::find_if(appliedPatches_.begin(), appliedPatches_.end(),
                                  [&](const auto& item) { return item.id == strategy->id(); });
        if (record != appliedPatches_.end()) {
            // Someone restored the entry point, but a previously allocated stub
            // may still need to be released before installing another one.
            if (!strategy->undo(proc, *record)) {
                report.entries.push_back(makeResult(*strategy, state, PatchResultStatus::Failed,
                                                     "Previous patch still needs recovery"));
                report.success = false;
                continue;
            }
            appliedPatches_.erase(record);
        }
        AppliedPatch patch;
        const bool success = strategy->apply(proc, patch);
        if (!patch.originalBytes.empty()) appliedPatches_.push_back(std::move(patch));
        report.entries.push_back(makeResult(*strategy,
            success ? PatchState::PatchedByUs : PatchState::PatchedUnknown,
            success ? PatchResultStatus::Applied :
                (strategy->required() ? PatchResultStatus::Failed : PatchResultStatus::Skipped),
            success ? "applied" : "Write failed; will retry without discarding recovery data"));
        if (!success && strategy->required()) report.success = false;
    }
    return report;
}

PatchReport PatchManager::undo(HANDLE proc, const std::string& id) {
    PatchReport report;
    auto* strategy = findStrategy(id);
    if (!strategy) {
        report.entries.push_back({id, PatchResultStatus::Failed, PatchState::NotFound, "Patch id not found"});
        return report;
    }
    auto record = std::find_if(appliedPatches_.begin(), appliedPatches_.end(),
                              [&](const auto& item) { return item.id == id; });
    AppliedPatch live;
    live.id = id;
    const auto state = strategy->detect(proc);
    if (record == appliedPatches_.end() && state != PatchState::PatchedByUs) {
        report.success = state != PatchState::PatchedUnknown;
        report.entries.push_back(makeResult(*strategy, state, PatchResultStatus::Skipped,
                                             report.success ? "not patched" : "Unrecognized code; not restored"));
        return report;
    }
    if (record != appliedPatches_.end() && !sameProcess(proc, *record)) {
        report.entries.push_back(makeResult(*strategy, state, PatchResultStatus::Failed,
                                             "Process identity changed; restore refused"));
        return report;
    }
    if (!strategy->undo(proc, record == appliedPatches_.end() ? live : *record)) {
        report.entries.push_back(makeResult(*strategy, state, PatchResultStatus::Failed,
            "Restore failed or original bytes unavailable; restart NVIDIA overlay if this persists"));
        return report;
    }
    if (record != appliedPatches_.end()) appliedPatches_.erase(record);
    report.success = true;
    report.entries.push_back(makeResult(*strategy, PatchState::Unpatched, PatchResultStatus::Applied, "restored"));
    return report;
}

PatchReport PatchManager::undoAll(HANDLE proc) {
    PatchReport report;
    report.success = true;
    // Visit every strategy, including a hook detected from an earlier app run.
    for (auto it = strategies_.rbegin(); it != strategies_.rend(); ++it) {
        const auto result = undo(proc, (*it)->id());
        report.entries.insert(report.entries.end(), result.entries.begin(), result.entries.end());
        report.success &= result.success;
    }
    return report;
}
