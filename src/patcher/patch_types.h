#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class PatchState {
    Unpatched,
    PatchedByUs,
    PatchedUnknown,
    NotFound
};

enum class PatchType {
    ExportHook,
    SignaturePatch
};

enum class PatchResultStatus {
    Applied,
    AlreadyPatched,
    Skipped,
    Failed,
    NotFound
};

struct AppliedPatch {
    std::string id;
    PatchType type = PatchType::ExportHook;
    uintptr_t targetAddress = 0;
    std::vector<uint8_t> originalBytes;
    std::vector<uint8_t> patchedBytes;
    uintptr_t trampolineAddress = 0;
    uint32_t processId = 0;
    uint64_t processCreated = 0;
};

struct PatchResultEntry {
    std::string id;
    PatchResultStatus status = PatchResultStatus::Failed;
    PatchState state = PatchState::Unpatched;
    std::string message;
};

struct PatchReport {
    std::vector<PatchResultEntry> entries;
    bool success = false;
};

struct PatchDefinition {
    std::string id;
    std::string description;
    PatchType type = PatchType::ExportHook;
    bool enabled = true;
    bool required = true;
    std::wstring module;
    std::string exportName;
    std::vector<uint8_t> stubBytes;
    std::vector<std::vector<std::optional<uint8_t>>> signatures;
    std::vector<uint8_t> patchBytes;
    size_t overwriteSize = 0;
};
