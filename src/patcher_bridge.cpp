#include "patcher.h"
#include "patcher/patch_config.h"
#include "patcher/patch_manager.h"
#include "patcher/shadowplay_target.h"
#include "patcher/utils.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>

namespace {
std::mutex mutex;
PatcherSnapshot snapshot{};
PatcherLogFn logger = nullptr;
bool preview = false;
bool reloadPending = true;
std::optional<bool> browserRequest;
std::wstring legacyConfigPath;
std::optional<PatchConfig> config;
std::optional<ShadowPlayTarget> target;
std::unique_ptr<PatchManager> manager;
ULONGLONG nextAttempt = 0;
std::string previousLog;

void publish(PatcherState state, const std::string& message) {
    snapshot.state = state;
    const auto wide = stringToWstring(message);
    wcsncpy_s(snapshot.detail, wide.c_str(), _TRUNCATE);
    std::string log = std::to_string(state) + ":" + std::to_string(snapshot.processId) + ":" + message;
    for (unsigned i = 0; i < snapshot.itemCount; ++i)
        log += ":" + std::to_string(snapshot.items[i].state);
    if (log != previousLog && logger) logger(message.c_str());
    previousLog = std::move(log);
}

void releaseTarget() {
    if (target) closeShadowPlayTarget(*target);
    target.reset();
    manager.reset();
    snapshot.processId = 0;
    nextAttempt = 0;
}

void updateItems(const PatchReport* report = nullptr) {
    snapshot.itemCount = 0;
    snapshot.browserAvailable = snapshot.browserEnabled = FALSE;
    if (!config) return;
    for (const auto& definition : config->patches) {
        auto& item = snapshot.items[snapshot.itemCount++];
        strncpy_s(item.id, definition.id.c_str(), _TRUNCATE);
        item.required = definition.required;
        item.state = definition.enabled ? PATCH_ITEM_WAITING : PATCH_ITEM_DISABLED;
        if (definition.id == "browser_detect") {
            snapshot.browserAvailable = TRUE;
            snapshot.browserEnabled = definition.enabled;
        }
        if (!report) continue;
        for (const auto& result : report->entries) {
            if (result.id != definition.id) continue;
            if (result.status == PatchResultStatus::Failed) item.state = PATCH_ITEM_FAILED;
            else if (result.state == PatchState::PatchedByUs) item.state = PATCH_ITEM_APPLIED;
            else if (result.status == PatchResultStatus::Skipped || result.status == PatchResultStatus::NotFound)
                item.state = PATCH_ITEM_SKIPPED;
            else item.state = PATCH_ITEM_WAITING;
        }
    }
}

std::string describe(const PatchReport& report, const char* action) {
    std::ostringstream text;
    text << action;
    for (const auto& entry : report.entries) text << "\n" << entry.id << ": " << entry.message;
    return text.str();
}

bool restore() {
    if (!target || !manager) return true;
    if (WaitForSingleObject(target->processHandle, 0) == WAIT_OBJECT_0) { releaseTarget(); return true; }
    const auto report = manager->undoAll(target->processHandle);
    updateItems(&report);
    if (!report.success) {
        publish(PATCHER_ERROR, describe(report, "Could not restore all patches; recovery records retained."));
        return false;
    }
    releaseTarget();
    return true;
}

bool loadConfig() {
    std::string error;
    if (browserRequest) {
        if (!setPatchEnabled(legacyConfigPath, "browser_detect", *browserRequest, error)) {
            publish(PATCHER_ERROR, error);
            browserRequest.reset();
            return false;
        }
        browserRequest.reset();
    }
    bool importedLegacy = false;
    auto loaded = loadPatchConfig(legacyConfigPath, error, &importedLegacy);
    if (!loaded) { publish(PATCHER_ERROR, error); return false; }
    if (importedLegacy && logger)
        logger("Imported patches.json into current-user settings. The old file is no longer needed.");
    config = std::move(loaded);
    updateItems();
    reloadPending = false;
    return true;
}
} // namespace

extern "C" void PatcherInitialize(PatcherLogFn log, BOOL isPreview) {
    std::lock_guard lock(mutex);
    logger = log;
    preview = isPreview;
    legacyConfigPath = resolveLegacyConfigPath();
    if (preview) {
        snapshot.browserAvailable = TRUE;
        snapshot.itemCount = 3;
        const char* ids[] = {"display_affinity", "module_enum", "browser_detect"};
        for (unsigned i = 0; i < 3; ++i) {
            strcpy_s(snapshot.items[i].id, ids[i]);
            snapshot.items[i].required = i < 2;
            snapshot.items[i].state = i < 2 ? PATCH_ITEM_WAITING : PATCH_ITEM_DISABLED;
        }
        publish(PATCHER_PREVIEW, "Interface preview; NVIDIA is not accessed.");
        return;
    }
    try {
        if (loadConfig()) publish(PATCHER_WAITING, "Waiting for the local desktop and ShadowPlay.");
    } catch (const std::exception& error) { publish(PATCHER_ERROR, error.what()); }
}

extern "C" void PatcherRequestBrowser(BOOL enabled) {
    std::lock_guard lock(mutex);
    if (preview) {
        snapshot.browserEnabled = enabled;
        snapshot.items[2].state = enabled ? PATCH_ITEM_WAITING : PATCH_ITEM_DISABLED;
        return;
    }
    browserRequest = enabled != FALSE;
    reloadPending = true;
    nextAttempt = 0;
}

extern "C" void PatcherGetSnapshot(PatcherSnapshot* out) {
    if (!out) return;
    std::lock_guard lock(mutex);
    *out = snapshot;
}

extern "C" void PatcherTick(BOOL enabled, PatcherPolicy policy, BOOL reload) {
    std::lock_guard lock(mutex);
    if (preview) return;
    snapshot.policy = policy;
    try {
        reloadPending |= reload != FALSE;
        if (target && WaitForSingleObject(target->processHandle, 0) == WAIT_OBJECT_0) {
            releaseTarget(); // Never reuse addresses or originals across process lifetimes.
            updateItems();
        }
        const ULONGLONG now = GetTickCount64();
        if (reload) nextAttempt = 0;
        if (reloadPending) {
            if (now < nextAttempt) return;
            if (!restore() || !loadConfig()) { nextAttempt = now + 10000; return; }
        }
        if (!enabled || policy != PATCH_ALLOWED) {
            if (!restore()) return; // Retry restoration on the next worker tick.
            updateItems();
            publish(enabled ? PATCHER_PAUSED : PATCHER_OFF,
                    enabled ? "Patch protection paused by the current rules." : "Patch protection is off; originals restored.");
            return;
        }
        if (now < nextAttempt) return;
        if (!config) { reloadPending = true; return; }
        if (std::none_of(config->patches.begin(), config->patches.end(), [](const auto& def) { return def.enabled; })) {
            publish(PATCHER_OFF, "All patches are disabled in the saved settings.");
            return;
        }
        if (!target) {
            std::string error;
            target = findShadowPlayProcess(error);
            if (!target) {
                publish(PATCHER_WAITING, error);
                return;
            }
            snapshot.processId = target->processId;
            manager = std::make_unique<PatchManager>(*config);
            if (!manager->loadStrategies(error)) { releaseTarget(); publish(PATCHER_ERROR, error); return; }
        }
        const auto report = manager->applyAll(target->processHandle);
        updateItems(&report);
        const bool partial = std::any_of(report.entries.begin(), report.entries.end(), [](const auto& entry) {
            return entry.state != PatchState::PatchedByUs;
        });
        publish(!report.success ? PATCHER_ERROR : partial ? PATCHER_PARTIAL : PATCHER_ACTIVE,
                describe(report, report.success ? "Patch check completed." : "Patch check failed; automatic retry remains active."));
        nextAttempt = now + (report.success ? 2000 : 10000);
    } catch (const std::exception& error) {
        publish(PATCHER_ERROR, error.what());
        nextAttempt = GetTickCount64() + 10000;
    }
}

extern "C" void PatcherShutdown() {
    std::lock_guard lock(mutex);
    if (preview) return;
    try {
        // A thread may briefly be executing at an entry point; allow it to leave.
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (restore()) return;
            Sleep(50);
        }
        if (logger) logger("Some patches could not be restored before exit. Restart the NVIDIA overlay to clear its process memory.");
        releaseTarget();
    } catch (const std::exception& error) {
        if (logger) logger(error.what());
        releaseTarget();
    }
}
